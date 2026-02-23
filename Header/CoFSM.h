#pragma once

#include <iostream>
#include <concepts>
#include <vector>
#include <optional>
#include <memory>
#include <coroutine>
#include <functional>
#include <map>

struct transfer_awaitable
{
    std::coroutine_handle<> continuation;
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept
    {
        // resume awaiting coroutine or if no coroutine to resume, return noop coroutine
        return continuation ? continuation : std::noop_coroutine();
    }
    void await_resume() noexcept {}
};

//-----------------------------
// co_awaitable methods
//-----------------------------
struct yield
{
    struct wait_timer
    {
        float time = 0.0f;
    };

    template<typename T>
    struct ret_value
    {
        T value;
    };
        
    struct ret_void { };

    struct change_state
    {
        std::string state;
    };
    
    // wait for duration
    static auto wait(float t) { return wait_timer{ t }; }
    
    // next frame
    static auto frame() { return wait(0); }

    // state change
    static auto state(const std::string& state) { return change_state{state}; }
        
    // manually return
    template<typename T>
    static auto ret(T&& v) { return ret_value<T>{ std::move(v)}; }
    
    static auto ret() { return ret_void{}; }
};


template<typename T=void>
struct fsm_coro;

struct CoFsm;
    
template<typename T>
struct fsm_coro
{
    struct promise_type
    {
        CoFsm* scheduler = nullptr;
        std::coroutine_handle<> awaiter_func;
        T* ret_ptr = nullptr; // this is to allow void type
        bool returned = false;
        
        // should not call delete on void*
        template<typename U> static void manual_delete(U* t) { if(t) delete t; }
        template<>           static void manual_delete<void>(void*){}
        
        ~promise_type()
        {
            manual_delete(ret_ptr);
            ret_ptr = nullptr;
            if(awaiter_func)
                awaiter_func.destroy();
//            fmt::print("destroyed\n");
        }
        
        // required interface
        fsm_coro get_return_object() { return fsm_coro { get_handle() }; }

        std::coroutine_handle<promise_type> get_handle()
        {
            return std::coroutine_handle<promise_type>::from_promise(*this);
        }

        void unhandled_exception() { std::rethrow_exception(std::move(std::current_exception())); }

        std::suspend_always initial_suspend() { return {}; }

        // final_suspend is when this coroutine ends, usually co_return
        auto final_suspend() noexcept;


        void return_void() {}
        
        auto await_transform(yield::wait_timer&& timer);
        
        template<typename U>
        auto await_transform(fsm_coro<U>&& coro);
        
        template<typename U>
        auto await_transform(yield::ret_value<U>&& ret);
        
        auto await_transform(yield::ret_void&& ret);
        
        auto await_transform(yield::change_state&& state);
    };

    fsm_coro(std::coroutine_handle<promise_type> h) : handle(h) {}
    
    std::coroutine_handle<promise_type> handle;
};

struct CoFsm
{
    CoFsm()
    {
        m_stateTable[""] = State {"", nullptr};
        m_runtime.state = &m_stateTable[""];
    }
    
    CoFsm(CoFsm&& other)
    {
        *this = std::move(other);
    }
    
    CoFsm& operator=(CoFsm&& other)
    {
        std::swap(m_runtime, other.m_runtime);
        std::swap(m_invokeList, other.m_invokeList);
        std::swap(m_destroyList, other.m_destroyList);
        std::swap(m_stateTable, other.m_stateTable);
        return *this;
    }
    
    ~CoFsm()
    {
        for(auto timer : m_invokeList)
            timer.handle.destroy();
        m_invokeList.clear();
        
        for(auto h : m_destroyList)
            h.destroy();
        m_destroyList.clear();
    }
    
    operator bool ()
    {
        return m_invokeList.size() > 0;
    }
    
    struct TimerInvoke
    {
        float time = 0.0f;
        std::coroutine_handle<> handle;
    };

    
    //-------------------------------
    struct State
    {
        std::string name;
        std::function<fsm_coro<>(const float&, const float&)> step;
        std::function<void()> exit;
    };
    
    struct StateRuntime
    {
        float dt = 0.0f;
        float elapsed = 0.0f;
        const State* state = nullptr;
    };
    
    void Step(float dt)
    {
        if(m_runtime.state)
        {
            m_runtime.dt = dt;
            m_runtime.elapsed += dt;
        }
        
        // timer and next frame tasks
        auto transientList = std::move(m_invokeList);
        m_invokeList.clear();
        for(auto& timer : transientList)
        {
            if(timer.time <= m_runtime.elapsed)
            {
                if(timer.handle && !timer.handle.done())
                {
                    timer.handle();
                }
            }
            else
            {
                m_invokeList.push_back(timer);
            }
        }
        
        //-----------------------------
        // we could schedule continuation in a similar loop to avoid scattered destroys
        // and hard to manage lifecycle of continuations
        //-----------------------------
        
        for(auto h : m_destroyList)
        {
            h.destroy();
        }
        m_destroyList.clear();
    }
    
    // should wrap in start
    template<typename U>
    fsm_coro<U>&& StartCoroutine(fsm_coro<U>&& coro)
    {
        coro.handle.promise().scheduler = this;
        ScheduleNextFrameResume(coro.handle);
        return std::move(coro);
    }
    
    // actual state step execution will be delayed to next frame
    void ChangeState(const std::string& newState)
    {
        assert(m_stateTable.count(newState) > 0 && "state table does not contain state");

        if(m_runtime.state && m_runtime.state->exit)
            m_runtime.state->exit();

        m_runtime.state = &m_stateTable[newState];
        m_runtime.elapsed = 0.0f;

        if(!m_runtime.state->step)
            throw "no step function";
        
        for(auto timer : m_invokeList)
            m_destroyList.push_back(timer.handle);
        m_invokeList.clear();

        StartCoroutine(m_runtime.state->step(m_runtime.dt, m_runtime.elapsed));
    }
    void ChangeOtherState(const std::string& newState)
    {
        if (m_runtime.state && m_runtime.state->name == newState) return;
		ChangeState(newState);
    }
    
    std::string DebugReportCurrentStateName()
    {
        if (m_runtime.state)
            return m_runtime.state->name;
        else return "null state";
    }
    
    void Run(const std::string& state)
    {
        ChangeState(state);
    }
    
    void ScheduleTimerResume(float duration, std::coroutine_handle<> h)
    {
        m_invokeList.push_back({ m_runtime.elapsed + duration, h });
    }
    
    void ScheduleNextFrameResume(std::coroutine_handle<> h)
    {
        ScheduleTimerResume(0, h);
    }

    void AddState(const State& state)
    {
        m_stateTable[state.name] = state;
    }
    
    void AddState(std::initializer_list<State> states)
    {
        for(auto& s : states)
            AddState(s);
    }
    
    StateRuntime m_runtime;
    
    std::vector<TimerInvoke>               m_invokeList;
    std::vector<std::coroutine_handle<>>   m_destroyList;
    std::unordered_map<std::string, State> m_stateTable;
};

template<typename X>
inline auto co_deref(X* ptr) { return *ptr; }

template<>
inline auto co_deref<void>(void* ptr){ }

template<typename T>
auto fsm_coro<T>::promise_type::final_suspend() noexcept
{
    //            assert(returned && "coroutine is not manually returned, may leak handle");
    if(!returned)
    {
        scheduler->m_destroyList.push_back(get_handle());
    }
    return transfer_awaitable{ std::exchange(awaiter_func, nullptr) };
}


template<typename T>
auto fsm_coro<T>::promise_type::await_transform(yield::wait_timer&& timer)
{
    scheduler->ScheduleTimerResume(timer.time, get_handle());
    return std::suspend_always{};
}
    
template<typename T>
template<typename U>
auto fsm_coro<T>::promise_type::await_transform(fsm_coro<U>&& coro)
{
    auto cont_handle = coro.handle;
    cont_handle.promise().awaiter_func = get_handle();
    cont_handle.promise().scheduler = scheduler;
    
    struct cont_awaitable
    {
        std::coroutine_handle<typename fsm_coro<U>::promise_type> cont_handle;
        
        bool await_ready() noexcept { return false; }
        // symetric transfer and immediately execute
        auto await_suspend(std::coroutine_handle<> self) noexcept { return cont_handle; }
        auto await_resume() noexcept { return co_deref(cont_handle.promise().ret_ptr); }
    };
    return cont_awaitable { cont_handle };
}

template<typename T>
template<typename U>
auto fsm_coro<T>::promise_type::await_transform(yield::ret_value<U>&& ret)
{
    ret_ptr = new U(std::move(ret.value));
    returned = true;
    scheduler->m_destroyList.push_back(get_handle());
    return transfer_awaitable { std::exchange(awaiter_func, nullptr) };
}
    
template<typename T>
auto fsm_coro<T>::promise_type::await_transform(yield::ret_void&& ret)
{
    returned = true;
    scheduler->m_destroyList.push_back(get_handle());
    return transfer_awaitable { std::exchange(awaiter_func, nullptr) };
}

template<typename T>
auto fsm_coro<T>::promise_type::await_transform(yield::change_state&& state)
{
    returned = true;
    scheduler->m_destroyList.push_back(get_handle());
    scheduler->ChangeState(state.state);
    return std::suspend_always{};
}
