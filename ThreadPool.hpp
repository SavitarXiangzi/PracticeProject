#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <future>

class ThreadPool {
public:
    // 删除拷贝构造和赋值操作
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 单例访问方法  hardware_concurrency->系统支持的并发线程数
    static ThreadPool& GetInstance(int numThreads = std::thread::hardware_concurrency()) {
        static ThreadPool instance(numThreads);
        return instance;
    }

    // 构造函数
    ThreadPool(int numThreads = std::thread::hardware_concurrency()) : stop(false) {
        init(numThreads);
    }

    // 析构函数
    ~ThreadPool() {
        shutdown();
    }

    // 初始化线程
    void init(int numThreads) {
        for (int i = 0; i < numThreads; i++) {
            threads.emplace_back([this]() {
                workerThread();
                });
        }
    }

    // 添加任务到队列
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type> {

        using return_type = typename std::result_of<F(Args...)>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mtx);

            // 不允许在停止后添加新任务
            if (stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }

            tasks.emplace([task]() { (*task)(); });
        }

        condition.notify_one();
        return result;
    }

    // 关闭线程池
    void shutdown() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            stop = true;
        }
        condition.notify_all();

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    // 获取线程数量
    size_t getThreadCount() const {
        return threads.size();
    }

    // 获取待处理任务数量
    size_t getPendingTaskCount() const {
        std::unique_lock<std::mutex> lock(mtx);
        return tasks.size();
    }

private:
    // 工作线程函数
    void workerThread() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mtx);
                condition.wait(lock, [this]() {
                    return !tasks.empty() || stop;
                    });

                if (stop && tasks.empty()) {
                    return;
                }

                task = std::move(tasks.front());
                tasks.pop();
            }

            try {
                task();
            }
            catch (const std::exception& e) {
                std::cerr << "ThreadPool task exception: " << e.what() << std::endl;
            }
        }
    }

private:
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks;

    mutable std::mutex mtx;
    std::condition_variable condition;

    bool stop;
};

// 测试函数
void testTask(int id, const std::string& name) {
    std::cout << "Task " << id << " (" << name << ") started" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "Task " << id << " (" << name << ") completed" << std::endl;
}

int computeSquare(int x) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return x * x;
}

int main() {
    // 使用方法1：单例模式
    std::cout << "=== 使用单例模式 ===" << std::endl;
    ThreadPool& pool1 = ThreadPool::GetInstance(4);

    std::vector<std::future<int>> results;
    for (int i = 1; i <= 8; i++) {
        results.emplace_back(
            pool1.enqueue(computeSquare, i)
        );
    }

    // 获取结果
    for (size_t i = 0; i < results.size(); i++) {
        std::cout << "Result " << (i + 1) << ": " << results[i].get() << std::endl;
    }

    std::cout << "\n=== 使用普通对象 ===" << std::endl;

    // 使用方法2：直接创建对象
    ThreadPool pool2(3);

    for (int i = 0; i < 6; i++) {
        pool2.enqueue([i]() {
            printf("Lambda task %d is running\n", i);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            printf("Lambda task %d is done\n", i);
            });
    }

    // 等待一段时间让任务执行
    std::this_thread::sleep_for(std::chrono::seconds(3));

    std::cout << "\n剩余任务数量: " << pool2.getPendingTaskCount() << std::endl;
    std::cout << "线程数量: " << pool2.getThreadCount() << std::endl;

    // 析构函数会自动调用shutdown
    return 0;
}
