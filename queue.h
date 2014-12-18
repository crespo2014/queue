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

#ifdef _NDEBUG
#define invariant(x)
#else
#define invariant(x) \
    assert(x)
#endif


using namespace std;

/**
 * Block of data or buffer
 */
class buffer
{
    const size_t size_;      // size of allocated buffer
    uint8_t* const buffer_;
public:
    buffer(uint8_t* buffer, size_t size) :
            size_(size), buffer_(buffer)
    {

    }
    size_t getSize() const
    {
        return size_;
    }
};

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
    const size_t size_;
    void* const page_;
public:
    page(size_t size) :
            size_((size + getpagesize() - 1) / getpagesize()), page_(mmap(0, size_, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
    {
        if (page_ == MAP_FAILED)
        {
            throw bad_alloc();
        }
    }
    ~page()
    {
        if (page_ != MAP_FAILED)
        {
            if (munmap(page_, size_) == -1)
            {
                //error log
            }
        }
    }
    void* get() const
    {
        return page_;
    }
    size_t size() const
    {
        return size_;
    }
    page(const page&) = delete;
    page(const page&& p) :
            size_(p.size_), page_(p.page_)
    {

    }
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
#if 0
class sfp_queue
{
    /**
     * each page will start with this
     */
    struct s_page_header
    {
        size_t used_;   ///< memory used or fill up
        uint8_t data[1] __attribute__ ((aligned));///<data start here
    };
    /**
     * class to make a list add next to base class
     */
    //forwar list
    class buffer: public page
    {
        // friend class sfp_queue;
        buffer* next_;
    public:
        buffer(size_t size) :
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
            if ((pos > size_ - offsetof(s_page_header, data)) || (size > size_ - offsetof(s_page_header, data) - pos))//TODO use assert for optimization, then force to go debug mode to check error
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
    buffer* blocks_ = nullptr;
    buffer* free_blocks_ = nullptr;
    // current write position will be hold from outside,
    mutex mutex_;///< global mutex use for move on reader and writers to different block, disconnect/connect readers
    condition_variable cv_;///< condition variable for data available
    buffer* reader_ = nullptr;///< rd_pos = 0 no reader rd_pos !=0 waiting for first block of data, set by writer at first block creation
    buffer* writer_ = nullptr;
    size_t wr_pos_ = 0;
    size_t rd_pos_ = 0;///< if 0 means no reader, offset(data) means waiting for reader
    volatile unsigned rd_status_ = 0;// 0 - no reader, // 1 - reader on // 2 - reader waiting
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
        if (rd_status_ == 1)// reading status
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
            buffer* buffer = blocks_;
            blocks_ = buffer->next_;
            delete buffer;
        }
        while (free_blocks_ != nullptr)
        {
            buffer* buffer = free_blocks_;
            free_blocks_ = buffer->next_;
            delete buffer;
        }
    }
    /**
     * Move reader to the next block
     */
    void ReaderGoNext()
    {
        unique_lock<std::mutex> lock(mutex_);
        buffer* b = reader_;
        reader_ = reader_->next_;
        rd_pos_ = 0;
        b->next_ = free_blocks_;
        free_blocks_ = b;
    }
};
#endif

/**
 * Queue interface
 */
class IQueue
{
public:
    class Block
    {
        const uint8_t* const ptr;
        const size_t len;
    };
    virtual void ReaderStart() = 0;
    virtual void ReaderEnd() = 0;
    virtual bool Allocate(size_t len) = 0;
    void write(void* d, size_t len);
    void Commit();
    void RollBack();
    Block peek();
    void get(Block& b);     // consolidate read
    void wait();bool wait_ms(size_t ms);
    // template operator functions
};

/**
 * Fast queue.
 * atomic used counter.  fast available calculation
 * Next is an structure - ptr + size for fast write, Updated when size go 0.
 * Reader will disable writter side.
 * set next 0
 * set wr 0
 * set rd 0
 *
 * reader will test used, before do anything
 *
 * Writer do this secuence.
 * inc next, if was 0 then reset to 0
 * if next == 0 ignore
 * if wr == null ignore
 * if rd == null nothing else if wr == null then start.
 * if (used == 0) move rd to begin.
 *
 * if wr > rd then space until end
 * if wr == size and used != size -1 then go 0.
 *
 *  Reader variables
 *  Used_   - if zero do not continue
 *  rd_ptr  - nullptr no reader set yet
 *
 *  Writer variables
 *  use next and size, update it ,
 *  if size == 0 then, reset
 *  if next = nullptr then reset
 *
 *  Alloc
 *  if next == 0 && rd == 0 ignore.
 *  next == 0 reader != 0 , start again
 *  if (rd > wr) adjust next
 *
 *  variable usability.
 *  test used, test wr
 *  next_size update
 *
 *  writer
 *  next ++ //check zero for reset
 *  next = start
 *  write = next    // chek zero on wr for reset
 *  used++
 *
 *  reader
 *  rd = 0 OFF
 *  rd = start STARTING if next = 0
 *
 *  use exchange to test previous values
 *  OFF rd =0 , f =0 , next =0
 *  ON  used=0, f=1
 *
 *  writer
 *  push if f = 0 or next = 0, quit no way to push data until next alloc
 *  alloc if f==0 ,if (rd != 0) reset, quit
 *
 * volatile is fine if we are not required for atomic updates.
 * that is means it can be modified from outside.
 * best use atomic with memory barrier.
 */
class FastQueue: public IQueue
{

    const size_t size_;      // size of allocated buffer
    uint8_t* const buffer_;  // pointer to allocated memory

#if 0
    atomic<uint8_t> writing_;
    atomic<uint8_t> reading_;
    atomic<uint8_t*> rd_ptr_;
    atomic<uint8_t*> wr_ptr_;
#else
    volatile uint8_t writing_;
    volatile uint8_t reading_;
    volatile uint8_t* rd_ptr_;
    volatile uint8_t* wr_ptr_;
    volatile bool waiting_ = false;
#endif
    atomic_size_t used_;
    uint8_t* next_ptr_;
    size_t next_size_;
    mutex mutex_;               ///< global mutex use for move on reader and writers to different block, disconnect/connect readers
    condition_variable cv_;     ///< condition variable for data available

    /**
     * Reset all pointer to beggining
     */
    void reset()
    {
        wr_ptr_ = buffer_;
        rd_ptr_ = buffer_;
        next_ptr_ = buffer_;
        next_size_ = size_ - 1;
        used_ = 0;
        //asm volatile ("" : : : "memory");       // force all write operations
        writing_ = 1;
    }
public:
    FastQueue(size_t size) :
            size_(size), buffer_(new uint8_t[size])
    {
        reset();
        reading_ = 1;
    }
    void readerStart()
    {
        writing_ = 0;
        reading_ = 1;
    }
    void readerStop()
    {
        reading_ = 0;
        writing_ = 0;
    }
    /**
     * Reader peek data
     */
    buffer Peek()
    {
        invariant(reading_ == 1);
        uint8_t* p = nullptr;
        size_t s = 0;
        if ((writing_ == 1) && (used_ != 0))
        {
            p = const_cast<uint8_t*>(wr_ptr_);
            if (p > rd_ptr_)
                s = p - rd_ptr_;
            else
                s = buffer_ + size_ - rd_ptr_ - (p == buffer_ ? 0 : 1);
            p = const_cast<uint8_t*>(rd_ptr_);
        }
        return
        {   p,s};
    }
    /**
     * wait for data
     */
    void wait()
    {
        if ((writing_ == 0) || (used_ == 0))
        {
            unique_lock<std::mutex> lock(mutex_);
            waiting_ = true;
            cv_.wait(lock);
            waiting_ = false;
        }
    }
    /*
     * Reader remove data from queue
     */
    void Pop(buffer& b)
    {
        invariant(reading_ == 1);
        rd_ptr_ += b.getSize();
        if (rd_ptr_ == buffer_ + size_)
            rd_ptr_ = buffer_;
        used_ -= b.getSize();
    }
    /**
     * Allocate size for a write operation
     * @return true - the size was successfully allocated
     *  false - there is not enought room
     */
    bool allocate(size_t size)
    {
        if ((writing_ == 0) || ((used_ == 0) && (next_ptr_ != buffer_)))
            reset();
        if (reading_ == 1)
        {
            if (size > next_size_)
            {
                if (size < size_ - used_)
                    return false;
                uint8_t* p = const_cast<uint8_t*>(rd_ptr_);
                if (p >= next_ptr_)
                    next_size_ = p - next_ptr_;
                else
                {
                    next_size_ = buffer_ + size - next_ptr_;
                    if (p == buffer_)
                        next_size_--;
                }
            }
        }
        return true;
    }
    /**
     * Push data to the queue
     * Data is not ready until commit or rollback is called
     */
    void Push(const void* dt, size_t size)
    {
        if (writing_ && reading_)
        {
            size_t len = (size > next_size_) ? next_size_ : size;
            while (len)
            {
                memcpy(next_ptr_, dt, len);
                size -= len;
                next_size_ -= len;
                dt = reinterpret_cast<const uint8_t*>(dt) + len;
                if (next_ptr_ == buffer_ + size_)
                {
                    next_ptr_ = buffer_;
                    next_size_ = (rd_ptr_ - buffer_);
                }
                else
                {
                    if (len != 0)
                        throw bad_alloc();
                }
            }
        }
    }
    void Commit()
    {
        size_t count = (wr_ptr_ <= next_ptr_) ? next_ptr_ - wr_ptr_ : size_ - (wr_ptr_ - next_ptr_);
        wr_ptr_ = next_ptr_;
        used_ += count;
        if (waiting_)
            cv_.notify_one();
    }
    virtual ~FastQueue()
    {
        delete[] buffer_;
    }
}
;
/**
 * Standard queue using only one buffer that hold rd, wr pointer
 * added notification when more than 4KB has been written
 * consumer read at interval to peek less than 4kb data
 *
 * less switch queue
 */
class Queue4: public page
{
    struct tpage_hdr
    {
        uint32_t wr_pos;
        uint32_t rd_pos;
        uint32_t rd_last;   // no circular queue. if rd > wr then last is the last data
        uint8_t data[1];
    };
    volatile bool writing_ = false;      // 0 means reset is necessary, 1 - writter ready
    volatile bool reading_ = false;      // 0 no reader online
    volatile bool waiting_ = false;
    tpage_hdr * const buffer_;
    atomic_size_t used_{0};
    uint8_t* next_ptr_;
    size_t next_size_;
    mutex mutex_;               ///< global mutex use for move on reader and writers to different block, disconnect/connect readers
    condition_variable cv4kb_;     ///< condition variable for data available
    /**
     * Get max size available on buffer after header
     */
    size_t max_size() const
    {
        return (size() - offsetof(tpage_hdr, data) - 1);
    }
    /**
     * Reset all pointer to beggining
     */
    void reset()
    {
        buffer_->wr_pos = buffer_->rd_pos = offsetof(tpage_hdr, data);
        next_ptr_ = buffer_->data;
        next_size_ = max_size();
        used_ = 0;
        writing_ = true;
    }
public:
    Queue4(size_t size) :
            page(size), buffer_(reinterpret_cast<tpage_hdr *>(page::get())), next_ptr_(buffer_->data), next_size_(max_size())
    {
        buffer_->wr_pos = buffer_->rd_pos = offsetof(tpage_hdr, data);
        buffer_->rd_last = max_size();
    }
    /**
     * Reader start reading
     */
    void startReader()
    {
        invariant(reading_ == 0);
        writing_ = false;
        reading_ = true;
    }
    /**
     * Reader is disconnect
     */
    void endReader()
    {
        writing_ = false;
        reading_ = false;
    }
    /**
     * Reader peek data
     */
    buffer Peek()
    {
        invariant(reading_ == 1);
        size_t s = 0;
        if ((writing_ == 1) && (used_ != 0))
        {
            size_t w = buffer_->wr_pos;
            size_t r = buffer_->rd_pos;
            if (w >= r)
                s = w - r;
            else
                s = buffer_->rd_last - r;
        }
        return
        {   buffer_->data + buffer_->rd_pos,s};
    }
    /*
     * Reader remove data from queue
     */
    void Pop(size_t count)
    {
        invariant(reading_ == 1);
        buffer_->rd_pos += count;
        if (buffer_->rd_pos == buffer_->rd_last)
            buffer_->rd_pos = 0;
        used_ -= count;
    }
    /**
     * wait for data
     */
    void wait()
    {
        if ((writing_ == 0) || (used_ == 0))
        {
            unique_lock<std::mutex> lock(mutex_);
            waiting_ = true;
            cv4kb_.wait(lock);
            waiting_ = false;
        }
    }
    /**
     * Wait for a time
     */
    void wait_for(unsigned time_ms)
    {
        if ((writing_ == 0) || (used_ == 0))
        {
            unique_lock<std::mutex> lock(mutex_);
            waiting_ = true;
            cv4kb_.wait_for(lock, std::chrono::milliseconds(time_ms));
            waiting_ = false;
        }
    }
    /**
     * Allocate size for a write operation
     * The space need to be continuous allocate after write or before read
     * @return true if there is space or not reader is connected
     *
     */
    bool allocate(size_t size)
    {
        if ((writing_ == 0) || ((used_ == 0) && (next_ptr_ != buffer_->data)))
            reset();
        if (reading_ == 1)
        {
            if (size > next_size_)      // appear no space available
            {
                size_t w = buffer_->wr_pos;
                size_t r = buffer_->rd_pos;
                if (w>=r)
                {
                    next_size_ = max_size() - w; // recalculate space at the end
                    if (next_size_ < size)
                    {
                        return false; //no space at the end
                    }
                    if (r < size)
                        return false; //no space at the begin
                    //buffer_->rd_last = buffer_->wr_pos;       // when commit make wr = next and set last is next < wr
                    next_ptr_ = buffer_->data;
                    next_size_ = r;
                    //buffer_->wr_pos = 0;
                }
                else
                {
                    next_size_ = r - w;
                    if (next_size_ < size)
                        return false;
                }
            }
        }
        return true;
    }
    /**
     * Push data to the queue
     * Data is not ready until commit or rollback is called
     */
    void Push(const void* dt,const size_t len)
    {
        if (writing_ && reading_)
        {
            assert(next_size_ >= len);
            memcpy(next_ptr_, dt, len);
            next_size_ -= len;
            next_ptr_ += len;
        }
    }
    /**
     * Commit data already written. this functions is call after get
     * @param [in] count - how many data has been written
     */
    void Commit()
    {
        size_t len = 0;
        if (next_ptr_ >= buffer_->data + buffer_->wr_pos)
        {
            len = next_ptr_ - buffer_->data + buffer_->wr_pos;
            assert(buffer_->wr_pos + len <= max_size());
            buffer_->wr_pos += len;
            used_ += len;
        }
        else
        {
            len = next_ptr_ - buffer_->data;
            buffer_->rd_last = buffer_->wr_pos;
            buffer_->wr_pos = len;
        }
        if (used_ > 1024*4)
            cv4kb_.notify_one();
    }
    /**
     * Rool back any pending write
     */
    void RollBack()
    {
        size_t r = buffer_->rd_pos;
        next_ptr_ = buffer_->data + buffer_->wr_pos;
        if (r < buffer_->wr_pos)
            next_size_ =  max_size() - buffer_->wr_pos;
        else
            next_size_ = r - buffer_->wr_pos;
    }
    /**
     * Check for readers. call this function to avoid written to the queue
     * return true - there is a reader.
     */
    bool isReading() const
    {
        return reading_;
    }
};

#endif /* QUEUE_H_ */
