#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <future>
#include <atomic>
#include <algorithm>
#include <type_traits>

class ThreadPool {
public:
    // 删除拷贝构造和赋值操作
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    // 单例访问方法
    static ThreadPool& GetInstance() {
        static ThreadPool instance;
        return instance;
    }
    
    // 构造函数
    ThreadPool() : stop(false), minThreads(5), maxThreads(20), 
                   busyThreads(0), totalTasks(0) {
        init(minThreads);
    }
    
    // 析构函数
    ~ThreadPool() {
        shutdown();
    }
    
    // 初始化线程
    void init(int numThreads) {
        std::unique_lock<std::mutex> lock(mtx);
        
        if (numThreads < minThreads) numThreads = minThreads;
        if (numThreads > maxThreads) numThreads = maxThreads;
        
        // 创建新线程直到达到目标数量
        while (threads.size() < static_cast<size_t>(numThreads)) {
            threads.emplace_back([this]() {
                workerThread();
            });
        }
        
        currentThreadCount = threads.size();
        std::cout << "线程池初始化，当前线程数: " << currentThreadCount << std::endl;
    }
    
    // 添加任务到队列 - 使用 C++20 语法和 lambda 表达式
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<F, Args...>> {
        
        using return_type = std::invoke_result_t<F, Args...>;
        
        // 使用 lambda 表达式替代 std::bind
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f), 
             captured_args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                
                // C++17 结构化绑定 + std::apply 调用函数
                return std::apply([&](auto&&... args) {
                    return func(std::forward<decltype(args)>(args)...);
                }, std::move(captured_args));
            }
        );
        
        std::future<return_type> result = task->get_future();
        
        {
            std::unique_lock<std::mutex> lock(mtx);
            
            // 不允许在停止后添加新任务
            if (stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            
            // 使用 lambda 包装任务
            tasks.emplace([task = std::move(task)]() { 
                (*task)(); 
            });
            
            totalTasks++;
            
            // 动态调整线程数量
            adjustThreadCount();
        }
        
        condition.notify_one();
        return result;
    }
    
    // 动态调整线程数量
    void adjustThreadCount() {
        // 如果任务队列很长且有空闲线程容量，增加线程
        if (tasks.size() > threads.size() * 2 && threads.size() < maxThreads) {
            int threadsToAdd = std::min(
                static_cast<int>(tasks.size() / 2 - threads.size()),
                static_cast<int>(maxThreads - threads.size())
            );
            
            for (int i = 0; i < threadsToAdd; i++) {
                threads.emplace_back([this]() {
                    workerThread();
                });
            }
            
            currentThreadCount = threads.size();
            std::cout << "增加 " << threadsToAdd << " 个线程，当前线程数: " 
                      << currentThreadCount << std::endl;
        }
        // 如果线程空闲且数量超过最小值，减少线程
        else if (busyThreads < threads.size() / 2 && threads.size() > minThreads) {
            // 标记需要停止的线程数量
            int threadsToRemove = std::min(
                static_cast<int>(threads.size() - busyThreads - 1),
                static_cast<int>(threads.size() - minThreads)
            );
            
            // 通知线程退出
            for (int i = 0; i < threadsToRemove; i++) {
                condition.notify_one(); // 唤醒线程让其自然退出
            }
        }
    }
    
    // 关闭线程池
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            stop = true;
        }
        condition.notify_all();
        
        for (auto &t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        
        threads.clear();
        currentThreadCount = 0;
    }
    
    // 获取线程数量
    size_t getThreadCount() const {
        std::unique_lock<std::mutex> lock(mtx);
        return threads.size();
    }
    
    // 获取待处理任务数量
    size_t getPendingTaskCount() const {
        std::unique_lock<std::mutex> lock(mtx);
        return tasks.size();
    }
    
    // 获取繁忙线程数量
    size_t getBusyThreadCount() const {
        return busyThreads.load();
    }
    
    // 获取总任务数量
    size_t getTotalTasks() const {
        return totalTasks.load();
    }
    
    // 设置最小最大线程数
    void setThreadLimits(int min, int max) {
        std::unique_lock<std::mutex> lock(mtx);
        minThreads = std::max(1, min);
        maxThreads = std::max(minThreads, max);
        
        // 立即调整线程数量
        adjustThreadCount();
    }

private:
    // 工作线程函数
    void workerThread() {
        while (true) {
            std::function<void()> task;
            
            {
                std::unique_lock<std::mutex> lock(mtx);
                
                // 检查是否需要退出（停止或线程过多）
                if (stop || threads.size() > minThreads) {
                    // 如果任务为空且线程数超过最小值，这个线程可以退出
                    if (tasks.empty() && threads.size() > minThreads) {
                        // 从线程列表中移除当前线程
                        auto it = std::find_if(threads.begin(), threads.end(),
                            [](const std::thread& t) { return t.get_id() == std::this_thread::get_id(); });
                        
                        if (it != threads.end()) {
                            std::thread& currentThread = *it;
                            
                            // 如果线程可join，先分离然后从vector中移除
                            if (currentThread.joinable()) {
                                currentThread.detach();
                            }
                            threads.erase(it);
                            currentThreadCount = threads.size();
                            
                            std::cout << "线程退出，当前线程数: " << currentThreadCount << std::endl;
                        }
                        return;
                    }
                }
                
                condition.wait(lock, [this]() {
                    return !tasks.empty() || stop;
                });
                
                if (stop && tasks.empty()) {
                    return;
                }
                
                task = std::move(tasks.front());
                tasks.pop();
            }
            
            // 执行任务
            busyThreads++;
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "ThreadPool task exception: " << e.what() << std::endl;
            }
            busyThreads--;
        }
    }

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks;
    
    mutable std::mutex mtx;
    std::condition_variable condition;
    
    std::atomic<bool> stop;
    std::atomic<int> busyThreads;
    std::atomic<size_t> totalTasks;
    
    int minThreads;
    int maxThreads;
    size_t currentThreadCount;
};

// 测试函数
void testTask(int id, const std::string& name) {
    std::cout << "任务 " << id << " (" << name << ") 开始执行，线程ID: " 
              << std::this_thread::get_id() << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "任务 " << id << " (" << name << ") 完成" << std::endl;
}

int computeSquare(int x) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return x * x;
}

// 演示各种函数类型的调用
class Calculator {
public:
    int multiply(int a, int b) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return a * b;
    }
    
    static int add(int a, int b) {
        return a + b;
    }
};

int main() {
    ThreadPool& pool = ThreadPool::GetInstance();
    
    std::cout << "初始线程数: " << pool.getThreadCount() << std::endl;
    
    // 测试 1: 普通函数
    std::cout << "\n=== 测试 1: 普通函数 ===" << std::endl;
    auto future1 = pool.enqueue(computeSquare, 5);
    std::cout << "computeSquare(5) = " << future1.get() << std::endl;
    
    // 测试 2: lambda 表达式
    std::cout << "\n=== 测试 2: Lambda 表达式 ===" << std::endl;
    auto future2 = pool.enqueue([](int a, int b) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return a + b;
    }, 10, 20);
    std::cout << "Lambda 10 + 20 = " << future2.get() << std::endl;
    
    // 测试 3: 成员函数
    std::cout << "\n=== 测试 3: 成员函数 ===" << std::endl;
    Calculator calc;
    auto future3 = pool.enqueue(&Calculator::multiply, &calc, 6, 7);
    std::cout << "Calculator::multiply(6, 7) = " << future3.get() << std::endl;
    
    // 测试 4: 静态成员函数
    std::cout << "\n=== 测试 4: 静态成员函数 ===" << std::endl;
    auto future4 = pool.enqueue(&Calculator::add, 15, 25);
    std::cout << "Calculator::add(15, 25) = " << future4.get() << std::endl;
    
    // 测试 5: 大量任务测试动态调整
    std::cout << "\n=== 测试 5: 大量任务测试动态调整 ===" << std::endl;
    std::vector<std::future<int>> results;
    
    for (int i = 0; i < 30; i++) {
        results.emplace_back(
            pool.enqueue([i]() -> int {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return i * i;
            })
        );
    }
    
    // 监控线程状态
    auto monitor = std::async(std::launch::async, [&pool]() {
        for (int i = 0; i < 8; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::cout << "监控 - 线程数: " << pool.getThreadCount() 
                      << ", 繁忙线程: " << pool.getBusyThreadCount()
                      << ", 待处理任务: " << pool.getPendingTaskCount() << std::endl;
        }
    });
    
    // 获取结果
    for (size_t i = 0; i < results.size(); i++) {
        auto result = results[i].get();
        if (i % 10 == 0) {
            std::cout << "结果[" << i << "] = " << result << std::endl;
        }
    }
    
    monitor.get();
    
    std::cout << "\n最终状态:" << std::endl;
    std::cout << "线程数: " << pool.getThreadCount() << std::endl;
    std::cout << "繁忙线程: " << pool.getBusyThreadCount() << std::endl;
    std::cout << "待处理任务: " << pool.getPendingTaskCount() << std::endl;
    std::cout << "总任务数: " << pool.getTotalTasks() << std::endl;
    
    return 0;
}
