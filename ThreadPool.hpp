#include <iostream>
#include <thread>
#include <mutex>
#include <string>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>

class ThreadPool {
public:
	ThreadPool():stop(false) {};
	ThreadPool(const ThreadPool& pool) = delete;
	ThreadPool& operator=(const ThreadPool& pool) = delete;
		 
	static ThreadPool& GetInstance() {
		static ThreadPool pool;
		return pool;
	};

	void init(int numThreads)  {
		for (int i = 0; i < numThreads; i++)
		{
			threads.emplace_back([this]() {
				while (1)
				{
					std::unique_lock<std::mutex> lock(mtx);
					condition.wait(lock, [this]() {
						return !tasks.empty() || stop;
						});
					if (stop && tasks.empty())
					{
						return;
					}
					
					std::function<void()> task(std::move(tasks.front()));
					tasks.pop();
					lock.unlock();
					task();
				}
			});
		}
	};

	~ThreadPool() {
		{
			std::unique_lock<std::mutex> lock(mtx);
 			stop = true;
		}

		//通知所有线程执行
		condition.notify_all();
		for (auto &t : threads)
		{
			t.join();
		}
	}

	template<class T,class... Args>
	void enqueue(T &&t,Args&... args) {
		std::function<void()> task = 
			std::bind(std::forward<T>(t), std::forward<Args>(args)...);
		{
			std::unique_lock<std::mutex> lock(mtx);
			tasks.emplace(std::move(task));
		}
			condition.notify_one();
	}

private:
	//线程数组
	std::vector<std::thread> threads;
	//任务队列
	std::queue<std::function<void()>> tasks;

	std::mutex mtx;
	std::condition_variable condition;

	bool stop;
};

int main() {

	ThreadPool pools; 
	pools.init(5);
	for (size_t i = 0; i < 100; i++)
	{
		pools.enqueue([i] {

			printf("task : %d is start\n",i);
			//std::cout << "task : " << i << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(1));
			//std::cout << "task : " << i  << "Done!" << std::endl;
			printf("task : %d is Done\n", i);
			});

	}
	return 0;
}
