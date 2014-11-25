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
#include <ostream>
#include <unistd.h>
#include <sys/mman.h>
#include <memory>
#include <cstring>
#include <cstddef>

#include "Buffer.h"

using namespace std;
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
        std::atomic<unsigned> readers_;      ///< how many client are reading from this block
        volatile unsigned wr_pos_ = 0;          ///< next position to be written by the producer
        T data_[M];                             ///< elements on this block
        Block* next_ = nullptr;                 ///< next block on the list,
        Block() :
                readers_(0)
        {

        }
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
            std::lock_guard<std::mutex> lock(queue_.mutex_);
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
            if (rd_block_->wr_pos_ == rd_pos_)
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
            std::unique_lock<std::mutex> lock(queue_.mutex_);
            if (!closed_ && rd_block_->wr_pos_ == rd_pos_)
            {
                queue_.waiters_++;
                queue_.cv_.wait(lock);
                queue_.waiters_--;
            }
        }
        /**
         * Close this reader that means, exit from wait condition
         */
        void close()
        {
            closed_ = true;
            queue_.cv_.notify_all();
        }
        /**
         * Move reader to next block
         */
        void gotoNext()
        {
            std::lock_guard<std::mutex> lock(queue_.mutex_);
            rd_block_->readers_--;
            rd_block_ = rd_block_->next_;
            rd_block_->readers_++;
            rd_pos_ = 0;
        }
    };
    volatile unsigned waiters_;      ///< how many readers are waiting for more data.
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
    Queue() :
            waiters_(0)
    {
        assert(N > 1);
        for (unsigned i = 1; i + 1 < N; ++i)
        {
            blocks_[i].next_ = &blocks_[i + 1];
        }
        initWritter(*wr_block_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        // try in the free list
        if (free_ != nullptr)
        {
            wr_block_->next_ = free_;
            wr_block_ = wr_block_->next_;
            free_ = free_->next_;
            initWritter(*wr_block_);
        } // find a block without readers
        else if (begin_->readers_ == 0) //try the first block
        {
            wr_block_->next_ = begin_;
            wr_block_ = wr_block_->next_;
            begin_ = begin_->next_;
            initWritter(*wr_block_);
        }
        else
        {
            done = false;
            tmp = begin_;
            while ((tmp->next_ != nullptr) && (tmp->next_->next_ != nullptr))    //there is more elements and is not the last
            {
                // if the next element does not have readers then use it
                if (tmp->next_->readers_ == 0)
                {
                    wr_block_->next_ = tmp->next_;
                    wr_block_ = wr_block_->next_;
                    tmp->dropped_ += tmp->next_->wr_pos_;
                    tmp->next_ = tmp->next_->next_;
                    initWritter(*wr_block_);
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
        assert(wr_block_->wr_pos_ + count <= M);
        wr_block_->wr_pos_ += count;
        //signal all waiting readers
        if (waiters_)
            cv_.notify_all();
    }
    /**
     * Get a reader for this queue
     */
    Reader getReader()
    {
        return
        {   *this};
    }
    /**
     * print out stats of the queue
     */
    std::ostream & stat(std::ostream & os)
    {
        os << N << " Blocks " << M << " items per block " << std::endl;
        unsigned c = 0;
        Block* p;
        p = free_;
        while (p)
        {
            p = p->next_;
            c++;
        }
        if (c != 0)
            os << c << " frees " << std::endl;
        p = begin_;
        c = 0;
        while (p)
        {
            c++;
            os << c << " " << p->wr_pos_ << " items ";
            if (p->readers_ != 0)
                os << std::endl << " " << p->readers_ << " readers ";
            if (p->dropped_ != 0)
                os << std::endl << " " << p->dropped_ << " items drop ";
            p = p->next_;
            os << std::endl;
        }

        return os;
    }
};

/**
 * Page allocator
 * Allocated the nearest page that can hold the required memory
 */
class page
{
protected:
    void* page_;
    size_t size_;
public:
    page(size_t size)
    {
        int psize = getpagesize();
        size_ = (size + psize - 1) / psize;
        page_ = mmap(0, size_, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page_ == MAP_FAILED)
        {
            throw bad_alloc();
        }
    }
    ~page()
    {
        if (munmap(page_, size_) == -1)
        {
            //error log
        }
    }
    page(const page&) = delete;
    page(const page&&) = delete;
    page& operator=(const page&) = delete;
    page& operator=(const page&&) = delete;
};

/**
 * One consumer, one producer small footprint queue
 * two operation modes.
 * 1 - no reader no data is produce. writter does not known
 * 2 - Buffer overflow only happens if there is reader on the first block.
 * Mutex is use to move on to a new block.
 *
 * All write operation are protected using mutex.
 * Reader only lock mutex when waiting for data and when do modifications.+
 *
 * empty is set by reader to indicate waiting condition
 * reader will do nothing until data become available via signal.
 * waiting is modified only inside mutex
 *
 * writer is able to modified reader data if waiting is true.
 */

class sfp_queue
{
    /**
     * each page will start with this
     */
    struct s_page_header
    {
        size_t used_;   ///< memory used or fill up
        uint8_t data[1] __attribute__ ((aligned));    ///<data start here
    };
    class block: public page
    {
        friend class sfp_queue;
        block* next_;
    public:
        block(size_t size) :
                page(size), next_(nullptr)
        {
            struct s_page_header* p = reinterpret_cast<s_page_header*>(page_);
            p->used_ = 0;
        }
        size_t getFree() const
        {
            return size_ - reinterpret_cast<s_page_header*>(page_)->used_;
        }
        template<class T>
        void write(const T& d, size_t pos)
        {
            write(&d, sizeof(T), pos);
        }
        void write(const void* d, size_t size, size_t pos)
        {
            struct s_page_header* p = reinterpret_cast<s_page_header*>(page_);
            // overflow situation is covered
            if ((pos > size_ - offsetof(s_page_header, data)) || (size > size_ - offsetof(s_page_header, data) - pos)) //TODO use assert for optimization, then force to go debug mode to check error
                throw bad_alloc();
            memcpy(p->data + pos, d, size);
        }
        void commit(size_t pos)
        {
            assert(pos <= size_ - offsetof(s_page_header, data));
            reinterpret_cast<s_page_header*>(page_)->used_ = pos;
        }
        size_t getUsed() const
        {
            return reinterpret_cast<s_page_header*>(page_)->used_;
        }
        /**
         * Get pointer to data from position
         */
        void* getPtr(size_t pos)
        {
            return reinterpret_cast<s_page_header*>(page_)->data + pos;
        }
    };

    size_t max_size_ = 0;
    size_t allocated_ = 0;
    block* blocks_ = nullptr;
    block* free_blocks_ = nullptr;
    // current write position will be hold from outside,
    mutex mutex_;               ///< global mutex use for move on reader and writers to different block, disconnect/connect readers
    condition_variable cv_;     ///< condition variable for data available
    block* reader_ = nullptr;    ///< rd_pos = 0 no reader rd_pos !=0 waiting for first block of data, set by writer at first block creation
    block* writer_ = nullptr;
    size_t wr_pos_ = 0;
    size_t rd_pos_ = 0;  ///< if 0 means no reader, offset(data) means waiting for reader
    volatile unsigned rd_status_ = 0; // 0 - no reader, // 1 - reader on // 2 - reader waiting
public:
    /**
     * Ctor
     * @max - maximum size of allocated memory for the queue
     */
    sfp_queue(size_t max) :
            max_size_(max)
    {

    }
    /**
     * The reader is disconnected, no more data will be produce until a new reader start
     */
    void ReaderStop()
    {
        lock_guard<std::mutex> lock(mutex_);
        reader_ = nullptr;
        rd_status_ = 0;
        writer_ = nullptr;
        reader_ = nullptr;
        FreeBlocks();
    }
    /**
     * Reader start
     * A new reader has started reading data
     */
    void ReaderStart()
    {
        lock_guard<std::mutex> lock(mutex_);
        rd_status_ = 2;     //only writer put this to 1
    }
    /**
     * Reader get a block of memory to be send
     */
    void getBlock(void* &p, size_t& size)
    {
        if (rd_status_ == 0)            // exception
            return;
        if (rd_status_ == 1)            // reading status
        {
            if ((reader_->getUsed() == rd_pos_) && (reader_->next_ != nullptr))
            {
                ReaderGoNext();
            }
            if (reader_->getUsed() != rd_pos_)  //data available
            {
                p = reader_->getPtr(rd_pos_);
                size = reader_->getUsed() - rd_pos_;
                return;
            }
            rd_status_ = 2;     // go to waiting status
        }
//        unique_lock<std::mutex> lock(mutex_);
//        cv_.wait(lock);
        size = 0;
    }
    void waitData()
    {
        unique_lock<std::mutex> lock(mutex_);
        while (rd_status_ == 2)
            cv_.wait(lock);
    }
    /**
     * Delete all memory blocks allocated
     */
    void FreeBlocks()
    {
        while (blocks_ != nullptr)
        {
            block* block = blocks_;
            blocks_ = block->next_;
            delete block;
        }
        while (free_blocks_ != nullptr)
        {
            block* block = free_blocks_;
            free_blocks_ = block->next_;
            delete block;
        }
    }
    /**
     * Move reader to the next block
     */
    void ReaderGoNext()
    {
        unique_lock<std::mutex> lock(mutex_);
        block* b = reader_;
        reader_ = reader_->next_;
        rd_pos_ = 0;
        b->next_ = free_blocks_;
        free_blocks_ = b;
    }
};

#endif /* QUEUE_H_ */
