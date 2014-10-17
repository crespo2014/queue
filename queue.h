/*
 * queue.h
 *
 *  Created on: 17 Oct 2014
 *      Author: lester
 */

#ifndef QUEUE_H_
#define QUEUE_H_

#include <exception>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <atomic>

/**
 * Multiples consumers one producer queue
 * The queue is split in N blocks containing M elements of type T
 *
 */
template<class T, int N = 10, int M = 100>
class Queue
{
    /**
     * Produce when the required space can not be supplied because is > M
     */
    class out_of_space: public std::exception
    {

    };
    /**
     * Block of data
     * All block will be part of a single link list.
     */
    class Block
    {
    public:
        unsigned dropped_ = 0;                  ///< how many data has been dropped between this block and the next
        std::atomic<unsigned> readers_{0};      ///< how many client are reading from this block
        volatile unsigned wr_pos_ = 0;          ///< next position to be written by the producer
        T data_[M];                             ///< elements on this block
        Block* next_ = nullptr;                 ///< next block on the list,
    };
    /**
     * Reader is object with information about a specific reader on the queue
     * @todo reader need a condition function to be validate inside wait to interup wait
     */
    class Reader
    {
        /// copy constructor not allowed
        Reader(const Reader&) = delete;
        /// assignment  not allowed
        Reader& operator=(const Reader&) = delete;
        /// move assignment  not allowed
        Reader& operator==(Reader&& r) = delete;
        Queue<T, N, M>& queue_;     ///< queue for read
        Block* rd_block_;           ///< current reading block
        unsigned rd_pos_;           ///<current read position
        bool closed_ = false;       ///< the current reader is been close
    public:
        /**
         * Move constructor implementation
         */
        Reader(Reader&& r) :
                queue_(r.queue_), rd_block_(r.rd_block_), rd_pos_(r.rd_pos_)
        {
            r.rd_block_ = nullptr;
        }
        /**
         * Construct a new reader from queue
         * @todo flow diagram
         */
        Reader(Queue<T, N, M>& queue) :
                queue_(queue)
        {
            // to start reading from a block or to move to a new one we need to get a lock
            std::lock_guard < std::mutex > lock(queue_.mutex_);
            rd_block_ = queue_.begin_;      // start reading from the begging of the queue. some data will be send again on  connections lost
            rd_pos_ = 0;
            rd_block_->readers_++;          // it is not possible lock a block outside mutex because producer would be in this block
        }
        /**
         * Reader destructor. decrement reader counter to release the block
         * a exception could be throw in this destructor it is not a good idea, but
         */
        ~Reader()
        {
            if (rd_block_ != nullptr)
            {
                rd_block_->readers_--;  // there is not problem removing reader outside mutex, operation is atomic
            }
        }
        /**
         * wait and a block of data from the queue
         * @param [out] count - number total of elements available
         * @param [out] dropped - number of bytes been dropped
         * @return null if a signal is produce without data
         */
        T* get(size_t& count, size_t& dropped)
        {
            count = 0;
            dropped = 0;
            // the read pointer is move to next position, block keep locked by reader
            if (rd_block_->wr_pos_ - rd_pos_ == 0)
            {
                if (rd_block_->next_ != nullptr)
                {
                    dropped = rd_block_->dropped_;
                    gotoNext();
                }
                if (rd_block_->wr_pos_ == rd_pos_)
                    waitData();
            }
            count = rd_block_->wr_pos_ - rd_pos_;
            T* ret = rd_block_->data_ + rd_pos_;
            rd_pos_ += count;
            return ret;
        }
        /**
         * Wait for data become available
         */
        void waitData()
        {
            std::lock_guard < std::mutex > lock(queue_.mutex_);
            if (!closed_ && rd_block_->wr_pos_ == rd_pos_)
            {
                waiters_++;
                cv_.wait(queue_.mutex_);
                waiters_--;
            }
        }
        /**
         * Close this reader that means, exit from wait condition
         */
        void close()
        {
            closed_ = true;
            queue_.notify();
        }

    };
    volatile unsigned waiters_ = 0;      ///< how many readers are waiting for more data.
    std::condition_variable cv_;    ///< condition variable to be notify when new data is produce
    std::mutex mutex_; ///< mutex use to mutual exclusion when moving to the next element on the list
    Block blocks_[N]; ///< memory block containing all data
    Block* begin_ = blocks_;     ///< first full block on the list.
    Block* free_ = blocks_ + 1;      ///< list of free blocks and startup this list contains all block
    Block* wr_block_ = blocks_;  ///< point to the block currently been written
public:
    /**
     * Constructor
     * Write pointer is initialise to the first block, rest of the block will be on free list
     */
    Queue()
    {
        (*blocks_).next_ = nullptr;
        Block* pblock = blocks_ + 1;
        ;
        for (unsigned i = N - 1; i != 0; --i)
        {
            pblock->next_ = pblock + 1;
            ++pblock;
        }
        pblock->next_ = nullptr;    //last block point to null
        initWritter(wr_block_);
    }
    /**
     * Initialize a block for start writting on it
     */
    void initWritter(Block& b)
    {
        b.dropped_ = 0;
        b.readers_ = 0;
        b.wr_pos_ = 0;
        b.next_ = nullptr;
    }
    /**
     * Get a free block.
     * Current writing block will point to a new free block
     */
    bool gotoNextFreeBlock()
    {
        bool done = true;
        Block* tmp;
        std::lock_guard < std::mutex > lock(mutex_);
        // try in the free list
        if (free_ != nullptr)
        {
            tmp = free_->next_;
            initWritter(*free_);
            wr_block_->next_ = free_;
            free_ = tmp;
        } // find a block without readers
        else if (begin_->readers_ == 0) //try the first block
        {
            tmp = begin_->next_;
            initWritter(*begin_);
            begin_ = tmp;
        }
        else
        {
            done = false;
            tmp = begin_;
            while ((tmp->next_ != nullptr) && (tmp->next_->next != nullptr))    //there is more elements and is not the last
            {
                // if the next element does not have readers then use it
                if (tmp->next_->readers == 0)
                {
                    Block* c = tmp->next_->next;
                    tmp->dropped_ += tmp->next_->wr_pos;
                    initWritter(*tmp->next_);
                    tmp->next_ = c;
                    done = true;
                    break;
                }
                tmp = tmp->next_;
            }
        }
        return done;
    }
    /**
     * Get a free block of data
     * @param [in] count - max elements requested
     */
    T* get(size_t count)
    {
        assert(count <= M);
        if (wr_block_->wr_pos_ + count > M)
        {
            if (!gotoNextFreeBlock())
                return nullptr;
        }
        return wr_block_->data_ + wr_block_->wr_pos_;
    }
    /**
     * Commit data already written. this functions is call after get
     * @param [in] count - how many data has been written
     */
    void Commit(size_t count)
    {
        assert(wr_block_->wr_pos_ + count < M);
        wr_block_->wr_pos_ += count;
        //signal all waiting readers
        if (waiters_)
            cv_.notify_all();
    }
};

#endif /* QUEUE_H_ */
