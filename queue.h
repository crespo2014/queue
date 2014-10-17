/*
 * queue.h
 *
 *  Created on: 17 Oct 2014
 *      Author: lester
 */

#ifndef QUEUE_H_
#define QUEUE_H_

#include <exception>

/**
 * Multiples consumers one producer queue
 * The queue is split in N blocks containing M elements of type T
 *
 */
template<class T,int N = 10,int M=100>
class Queue
{
    /**
     * Produce when the required space can not be supplied because is > M
     */
    class out_of_space : public std::exception
    {

    }
};


#endif /* QUEUE_H_ */
