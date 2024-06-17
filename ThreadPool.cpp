#include "ThreadPool.h"

#include <functional>
#include <thread>
#include <iostream>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 60;

ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , taskSize_(0)
    , idleThreadSize_(0)
    , curThreadSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
{}

ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;
    notEmpty_.notify_all();

    // 等待线程池里面所有的线程返回     两种状态:阻塞 正在执行任务中
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
}

void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState())
    {
        return;
    }
    poolMode_ = mode;
} 

bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

// 设置task任务队列上线阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程的上限阈值
void ThreadPool::setThreadSizeThreashHold_(int threashhold)
{
    if(checkRunningState()) return;     // 如果线程池已经启动了，就不能再更改了，因为这些需要提前设置好
    
    if(poolMode_ == PoolMode::MODE_CACHED)
    {
        threadSizeThreshHold_ = threashhold;
    }
}

// 开启线程池
void ThreadPool::start(int initThreadSize)
{
    // 设置线程池的运行状态
    isPoolRunning_ = true;
    // 记录初始线程个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // 创建线程对象
    for(int i=0;i<initThreadSize_;i++)
    {
        // 创建thread线程对象的时候，把线程函数给到thread线程对象  std::make_unique需要设置-std=c++14
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        // threads_.emplace_back(std::move(ptr));
        // 如果使用make_unique，需要std::move(),(这里好像是用到了左值引用)
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        // threads_.emplace_back([this]{
        //     this->threadFunc();
        // });
    }

    // 启动所有线程
    for(int i=0;i<initThreadSize_;i++)
    {
        threads_[i]->start();       // 需要去执行一个线程函数
        idleThreadSize_++;      // 记录初始空闲线程的数量
        std::cout << "start...." << initThreadSize_ << std::endl;
    }
} 

// 给线程池提交任务  提交任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 获取锁  锁会在当前代码执行完的右括号位置释放
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // 线程的通信 等待任务队列有空余
    // while(taskQueue_.size() == taskQueMaxThreshHold_)
    // {
    //     notFull_.wait(lock);    // 进入等待状态
    // }
    // 这句代码和上面的while循环是一个意思  因为是放入任务队列，所以用notFull_，只要任务队列不满都可以放
    if(!notFull_.wait_for(lock, std::chrono::seconds(1), 
        [&]()->bool { return taskQueue_.size() < (size_t)taskQueMaxThreshHold_;}))
    {
        // 表示notFull_等待1s，条件依然没有满足
        std::cerr << "task queue is full, submit task fail!" << std::endl;
        // return task->getResult();
        return Result(sp, false);
    }
    
    // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败
    /* 
    wait(与时间没有关系，需要在外面加一个循环满足条件) 
    wait_for（加了一个时间参数，在规定时间内等待） 
    wait_until（加一个最后的时间节点，在时间节点前等待事件）
    */
    // 如果有空余，把任务队列放入队列中
    taskQueue_.emplace(sp);
    taskSize_++;

    // 因为新放入了任务，任务队列肯定不空，在notEmpty_上进行通知,赶快分配线程执行任务
    notEmpty_.notify_all();

    // catched模式：任务处理比较紧急。场景：小而快的任务。需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
    if(poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
    {
        std::cout << "create new thread...." << std::endl;

        // 创建新线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        // 启动线程
        threads_[threadId]->start();
        // 修改线程个数相关的变量
        curThreadSize_++;
    }

    //返回任务的Result对象
    return Result(sp);
}

// 定义线程任务函数 从任务队列中消费任务
void ThreadPool::threadFunc(int threadId)   // 线程函数返回，线程就结束了
{
    auto lastTime = std::chrono::high_resolution_clock().now();
    
    while(isPoolRunning_)
    {
        std::shared_ptr<Task> task;
        {
            // 先获取锁  
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            // 等待notEmpty_条件   因为是从任务队列中取出任务，所以这里是notEmpty_，只要任务队列非空都可以取
            // 这里不需要等待时间，因为只要有任务就从里面取，队列里面的任务都是需要执行的
            std::cout << "tid: " << std::this_thread::get_id() << "尝试获取任务" << std::endl;
            
            // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程回收掉
            // 结束回收掉（超过initThreadSize_数量的线程要进行回收）
            // 当前时间 - 上一次线程执行的时间 > 60s
            // 每一秒返回一次  怎么区分：超时返回？还是有任务待执行返回
            // 任务队列里面没有任务就等待
            while(taskSize_ == 0)
            {
                if(poolMode_ == PoolMode::MODE_CACHED)
                    {
                    // 条件变量超时返回
                    if(std::cv_status::timeout == 
                    notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                        {
                            // 开始回收当前线程
                            // 记录线程数量的相关变量的值修改
                            // 把线程对象从线程列表容器中删除 没有办法匹配 threadfunc对应哪个thread对象
                            threads_.erase(threadId);  // std::this_thread::getid()这个是系统分配的id，threadId是我们给他的id
                            curThreadSize_--;
                            idleThreadSize_--;
                            exitCond_.notify_all();

                            std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
                            return ;
                        }
                    }
                }
                else
                {
                    // 等待notEmpty_条件
                    notEmpty_.wait(lock);
                }

                // 唤醒以后检查isPoolRunning_是否为true，检查是否析构 
                // 线程池要结束，回收线程资源
                if(!isPoolRunning_)
                {
                    threads_.erase(threadId);  // std::this_thread::getid()这个是系统分配的id，threadId是我们给他的id

                    exitCond_.notify_all();

                    return;
                }
            }

            idleThreadSize_--;

            std::cout << "tid: " << std::this_thread::get_id() << "获取任务成功" << std::endl;

            // 从任务队列中取一个任务出来
            task = taskQueue_.front();
            taskQueue_.pop();
            taskSize_--;
            // 如果依然有剩余任务，继续通知其他线程执行任务
            if(taskQueue_.size() > 0)
            {
                notEmpty_.notify_all();
            }

            // 此时就应该把锁释放掉 所以在这里弄了一个局部对象，出了右括号锁就会自动释放
            // 取出一个任务，进行通知  通知继续提交生产任务,生产任务用的是notFull_  wait和notify是一一对应的
            notFull_.notify_all();
        }

        // 当前线程负责执行这个任务
        if(task != nullptr)
        {
            // task->run();
            std::cout << "task->exec()..." << std::endl;
            task->exec();
        }
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now();  // 更新线程执行完调度的时间

    }

    threads_.erase(threadId);  // std::this_thread::getid()这个是系统分配的id，threadId是我们给他的id
    exitCond_.notify_all();
}

////////////////// 线程方法实现
// 静态成员变量需要在外部初始化
int Thread::generateId_ = 0;

// 线程构造
Thread::Thread(ThreadFunc func)
    : func_(func)
    , threadId_(generateId_)
{}

// 线程析构
Thread::~Thread()
{}

int Thread::getId() const
{
    return threadId_;
}
// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_, threadId_);    // C++11来说 线程对象t 和线程函数func
    t.detach();     // 设置分离线程 pthread_detach   pthread_t设置成分离线程
}

////////Result方法的实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : isValid_(isValid)
    , task_(task)
{
    task_->setResult(this);
}

Any Result::get()       // 用户调用的
{
    if(!isValid_)
    {
        return "";
    }

    sem_.wait();    // task任务如果没有执行完，这里会阻塞用户的线程
    return std::move(any_);
}

void Result::setVal(Any any)        // 谁调用的呢
{
    // 存储task的返回值
    this->any_ = std::move(any);
    sem_.post();    // 已经获取的任务的返回值，增加信号量资源
}

///////////Task方法实现   这里非常巧妙的实现可以返回任意类型
void Task::exec()
{
    if(result_ != nullptr)
    {
        std::cout << "exec..." << std::endl;
        result_->setVal(run());  // 这里发生多态调用
    }
}
  
void Task::setResult(Result* res)
{
    result_ = res;
}

Task::Task()
    : result_(nullptr)
{}