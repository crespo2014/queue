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

/**
 * Multiples consumers one producer queue
 * The queue is split in N blocks containing M elements of type T
 *
 */
template<class T, int N = 10, int M = 100>
class Queue {
    /**
     * Produce when the required space can not be supplied because is > M
     */
    class out_of_space : public std::exception {

    };
    /**
     * Block of data
     * All block will be part of a single link list.
     */
    class Block {
    public:
        unsigned dropped_;   ///< how many data has been dropped between this block and the next
        unsigned readers_;   ///< how many client are reading from this block
        unsigned wr_pos_;   ///< next position to be written by the producer
        T data_[M];         ///< elements on this block
        Block* next_;      ///< next block on the list,
    };
    bool waiting_ = false;      ///< true if there is a consumer waiting for more data.
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
    Queue() {
        (*blocks_).next_ = nullptr;
        Block* pblock = blocks_ + 1;;
        for (unsigned i=N-1;i!=0;--i)
        {
            pblock->next_ = pblock + 1;
            ++pblock;
        }
        pblock->next_ = nullptr;    //last block point to null
        initWritter(wr_block_);
    }
    /**
     * Initialize a block for start writting
     */
    void initWritter(Block& b)
    {
        b.dropped_ = 0;
        b.readers_ = 0;
        b.wr_pos_ = 0;
        b.next_ = nullptr;
    }
};

#endif /* QUEUE_H_ */
