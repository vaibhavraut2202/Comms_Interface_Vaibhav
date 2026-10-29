#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

#include "command_queue.hpp"
#include "dispatcher.hpp"
#include "operation_result.hpp"
#include "result_store.hpp"

class Worker
{
private:
    CommandQueue& commandQueue;
    Dispatcher& dispatcher;
    ResultStore& resultStore;
    std::thread workerThread;

    void process(Command command)
    {
        if (!resultStore.markProcessing(command.requestId))
        {
            std::cerr << "[Worker] Cannot transition request "
                      << command.requestId << " to PROCESSING" << std::endl;
            return;
        }

        try
        {
            resultStore.complete(command.requestId, dispatcher.dispatch(command));
        }
        catch (const std::exception& error)
        {
            const std::string message = std::string("Internal processing error: ")
                + error.what();
            std::cerr << "[Worker] " << message << std::endl;
            resultStore.complete(
                command.requestId,
                OperationResult::failure("INTERNAL_ERROR", message));
        }
        catch (...)
        {
            const std::string message = "Internal processing error: unknown exception";
            std::cerr << "[Worker] " << message << std::endl;
            resultStore.complete(
                command.requestId,
                OperationResult::failure("INTERNAL_ERROR", message));
        }
    }

    void run()
    {
        while (true)
        {
            std::optional<Command> command = commandQueue.pop();
            if (!command)
            {
                return;
            }
            process(std::move(*command));
        }
    }

public:
    Worker(CommandQueue& queue, Dispatcher& dispatcher, ResultStore& results)
        : commandQueue(queue),
          dispatcher(dispatcher),
          resultStore(results)
    {
    }

    ~Worker()
    {
        stop();
        join();
    }

    void start()
    {
        if (workerThread.joinable())
        {
            throw std::logic_error("Worker has already been started");
        }
        workerThread = std::thread(&Worker::run, this);
    }

    void stop()
    {
        commandQueue.close();
    }

    void join()
    {
        if (workerThread.joinable())
        {
            workerThread.join();
        }
    }
};
