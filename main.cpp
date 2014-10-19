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

#include "CppUTest/TestHarness.h"
#include "CppUTestExt/MockSupport.h"
#include "CppUTest/MemoryLeakDetectorNewMacros.h"

#include "CppUTest/CommandLineTestRunner.h"

TEST_GROUP(queue)
{

};

TEST(queue,all)
{
    size_t count, drop;
    Queue<char, 3, 5> q;
    auto r1 = q.getReader();
    auto r2 = q.getReader();
    auto r3 = q.getReader();
    q.stat(std::cout);
    CHECK(q.get(1) != nullptr);
    q.Commit(5);    // one block full
    CHECK(q.get(1) != nullptr);
    q.Commit(5);    // two block full
    CHECK(q.get(1) != nullptr);
    q.Commit(5);    // 3 block full
    CHECK(q.get(2) != nullptr);     // 5 drops for readers
    q.Commit(2);

    q.stat(std::cout);

    CHECK(r1.get(count, drop) != nullptr);  //read b 1
    CHECK(count == 5);
    CHECK(drop == 0);

    q.stat(std::cout);

    CHECK(r1.get(count, drop) != nullptr);
    CHECK(count == 5);
    CHECK(drop == 5);

    q.stat(std::cout);

    CHECK(r1.get(count, drop) != nullptr);
    CHECK(count == 2);
    CHECK(drop == 0);

    q.stat(std::cout);

    CHECK(r2.get(count, drop) != nullptr);      //read 1
    CHECK(count == 5);
    CHECK(drop == 0);

    q.stat(std::cout);

    //move reader to to second block
    CHECK(r2.get(count, drop) != nullptr); // go to 2 get 5 drop
    CHECK(count == 5);
    CHECK(drop == 5);

    q.stat(std::cout);

    // try to produce more data with all block lock
    CHECK(q.get(5) == nullptr);

    q.stat(std::cout);

    // move r2 to block 3
    CHECK(r2.get(count, drop) != nullptr);
    CHECK(count == 5);
    CHECK(drop == 0);

    q.stat(std::cout);

    // produce more data and r3 will lost block 2

    //move r3 to block 2
    CHECK(r3.get(count, drop) != nullptr);
    CHECK(count == 5);
    CHECK(drop == 0);

    CHECK(r3.get(count, drop) != nullptr);
    CHECK(count == 5);
    CHECK(drop == 0);

    std::thread t([&]()
    {   sleep(2);r1.close();});
    r1.get(count, drop);
    CHECK(count == 0);
    t.join();

    std::cout << "." << std::endl;

}
;

#ifdef CPP_UTEST
int main(int ac, char** av)
{
    return CommandLineTestRunner::RunAllTests(ac, av);
}
#else
int main()
{
    test();
    return 0;
}

#endif

