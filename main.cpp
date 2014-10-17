/*
 * main.cpp
 *
 *  Created on: 17 Oct 2014
 *      Author: lester
 */

#include "queue.h"

void test()
{
    size_t count,drop;
    Queue<char,3,5> q;
    auto r1 = q.getReader();
    auto r2 = q.getReader();
    auto r3 = q.getReader();
    q.get(1);
    q.Commit(2);

    r1.get(count,drop);
    r2.get(count,drop);
    r3.get(count,drop);
}

int main()
{
    test();
    return 0;
}


