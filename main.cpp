#include <iostream>
#include <chrono>
#include <thread>

#include "ThreadPool.h"
using namespace std;

// 运行命令  g++ ThreadPool.cpp main.cpp -o myThreadpool -lpthread -std=c++17
/*
有些场景，是希望能够获取线程得到返回值的
举例
1 + 。。。。 + 30000的和
thread1  1 + ..... + 10000
thread2  10001 + .... + 20000
....
main thread：给每个线程分配计算的区间，并等他们算完返回结果，合并最终的结果即可
*/

class Mytask:public Task
{
public:
    Mytask(int begin, int end)
        : begin_(begin)
        , end_(end)
    {}
    // 默认构造函数
    // Mytask() : begin_(0), end_(0) {}
    // 问题1：怎么设计run函数的返回值，可以表示任意类型的返回值
    // C++17 Any类型 可以接收任意的其他类型
    Any run()
    {
        std::cout << "begin threadFunc tid: " << std::this_thread::get_id() << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        int sum = 0;
        for(int i=begin_;i<=end_;i++)
        {
            sum += i;
        }
        std::cout << "end threadFunc tid: " << std::this_thread::get_id() << std::endl;
        return sum;
    }
private:
    int begin_;
    int end_;
};

int main()
{
    // 
    {
        ThreadPool pool;
        // 用户自己设置线程池的工作模式
        pool.setMode(PoolMode::MODE_CACHED);

        pool.start(8);
        
        // 如何设计这里的result机制呢  这里成功提交后会得到一个返回值，但是得不到任务完成后的结果
        Result res1= pool.submitTask(std::make_shared<Mytask>(1,11));
        // 这里要等待任务完成，才能得到结果
        int sum = res1.get().cast_<int>();      // 返回了一个Any类型 
        std::cout << sum << std::endl;
        // Master - Slave线程模型
        // Master线程用来分解任务，然后给各个Slave线程分配任务
        // 等待各个Slave
        Result res2 =pool.submitTask(std::make_shared<Mytask>(12,20));
        int sum2 = res2.get().cast_<int>();
        std::cout << sum + sum2 << std::endl;
        // pool.submitTask(std::make_shared<Mytask>(2,100));
        // pool.submitTask(std::make_shared<Mytask>(2,100));
        // pool.submitTask(std::make_shared<Mytask>(2,100));
        // 睡眠5秒
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::cout << "main over" << std::endl;
    getchar();
}
