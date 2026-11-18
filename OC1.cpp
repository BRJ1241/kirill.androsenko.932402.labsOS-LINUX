#include <chrono>
#include <clocale>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

struct NonSerializable 
{
    std::mutex m;
};

struct Event 
{
    int id;
    NonSerializable payload;
    explicit Event(int i):id(i){}
};

class Monitor 
{
public:
    bool provide(std::unique_ptr<Event> ev) 
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_free_.wait(lock, [this] {return !ready_ || stopping_;});
        if (stopping_) 
        {
            return false;
        }
        data_ = std::move(ev);
        ready_ = true;
        std::cout << "Поставщик: событие отправлено (id="<< data_->id << ")\n";
        std::cout.flush();
        cv_ready_.notify_one();
        return true;
    }

    std::unique_ptr<Event> consume() 
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_ready_.wait(lock, [this] {return ready_ || stopping_;});
        if (!ready_) 
        {
            return nullptr;
        }
        auto ev = std::move(data_);
        ready_ = false;
        std::cout << "Потребитель: событие обработано (id="<< ev->id << ")\n";
        std::cout.flush();
        cv_free_.notify_one();
        return ev;
    }

    void stop() 
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = true;
        cv_ready_.notify_all();
        cv_free_.notify_all();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_ready_;
    std::condition_variable cv_free_;
    bool ready_ = false;
    bool stopping_ = false;
    std::unique_ptr<Event> data_;
};

int main() 
{
    setlocale(LC_ALL, "Russian");
    constexpr int N = 5;
    Monitor monitor;
    std::thread producer([&] {
        for (int i = 1; i <= N; ++i) 
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto ev = std::make_unique<Event>(i);
            if (!monitor.provide(std::move(ev))) 
            {
                break;
            }
        }
        monitor.stop();
        });
    std::thread consumer([&] {while (auto ev = monitor.consume()) {}});
    producer.join();
    consumer.join();
    return 0;
}