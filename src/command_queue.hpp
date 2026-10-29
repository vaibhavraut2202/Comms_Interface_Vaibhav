#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "command.hpp"

class CommandQueue
{
private:
    std::queue<Command> queue;
    std::mutex mutex;
    std::condition_variable condition;
    bool closed = false;

public:
    bool push(Command command)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closed)
            {
                return false;
            }
            queue.push(std::move(command));
        }
        condition.notify_one();
        return true;
    }

    std::optional<Command> pop()
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this]()
        {
            return closed || !queue.empty();
        });

        if (queue.empty())
        {
            return std::nullopt;
        }

        Command command = std::move(queue.front());
        queue.pop();
        return command;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        condition.notify_all();
    }
};
