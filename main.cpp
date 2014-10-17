/*
 * main.cpp
 *
 *  Created on: 17 Oct 2014
 *      Author: lester
 */

#include "queue.h"
#include <thread>
#include <unistd.h>
#include <ostream>
#include <iostream>

void test()
{
    char* p;
    size_t count,drop;
    Queue<char,3,5> q;
    auto r1 = q.getReader();
    auto r2 = q.getReader();
    auto r3 = q.getReader();
    p = q.get(1);
    q.Commit(5);    // one block full
    p = q.get(1);
    q.Commit(5);    // two block full
    p = q.get(1);
    q.Commit(5);    // two block full
    p = q.get(2);       // drop 5 to readers

    r1.get(count,drop); // check count = 2
    r1.get(count,drop); // check count = 2

    r2.get(count,drop);
    r2.get(count,drop); // check count = 2

    r3.get(count,drop);
    r3.get(count,drop); // check count = 2

    std::thread t([&]() { sleep(5);r1.close();});
    r1.get(count,drop); // check count = 2
    t.join();

    std::cout <<"." <<std::endl;
}

int main()
{
    test();
    return 0;
}


