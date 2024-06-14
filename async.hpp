#ifndef CHZN_UTIL_ASYNC_HPP
#define CHZN_UTIL_ASYNC_HPP

#include <coroutine>
#include <exception>
#include <vector>

namespace chzn{
    template<typename T>
    struct async;
    namespace _detail{
        enum coroutine_state: unsigned char{
            awaiting,
            returned,
            throws,
        };
        struct awaiting_notifier_destructed{};// throw on co_await no longer return

        // helper class to release coroutine handle at correct time
        struct unowned_promise{
            struct promise_type{
                unowned_promise get_return_object(){return {handle_type::from_promise(*this)};}
                constexpr std::suspend_never initial_suspend()const noexcept{return {};}
                constexpr std::suspend_never final_suspend()const noexcept{return {};}
                static void unhandled_exception(){throw;}
                constexpr void return_void()const noexcept{}
            };
            using handle_type=std::coroutine_handle<promise_type>;
            handle_type coroutine;

            template<typename T>
            static unowned_promise adopt(T promise){
                try{
                    co_await promise;
                }catch (awaiting_notifier_destructed){}
            }
        };
    }

    using no_longer_awaitable=_detail::awaiting_notifier_destructed;

    template<typename T=void>
    struct async{
        struct promise_type{
            async<T> get_return_object(){return {handle_type::from_promise(*this)};}
            // suspend at start to make caller co_await this, then set await_by when this be co_await
            constexpr std::suspend_always initial_suspend()const noexcept{return {};}
            void return_value(T t){
                new(&value) T(std::move(t));
                state=_detail::returned;
            }
            void unhandled_exception(){
                new(&error) std::exception_ptr(std::current_exception());
                state=_detail::throws;
            }
            struct suspend_final{
                constexpr bool await_ready()const noexcept{return false;}
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle)const noexcept{
                    return handle.promise().await_by;
                }
                constexpr void await_resume()const noexcept{}
            };
            // suspend at final, then resume the caller
            constexpr suspend_final final_suspend()const noexcept{
                return {};
            }
            ~promise_type(){
                switch (state) {
                    case _detail::returned:
                        reinterpret_cast<T&>(value).~T();
                        break;
                    case _detail::throws:
                        error.~exception_ptr();
                        break;
                }
            }
            union {
                alignas(T) std::byte value[sizeof(T)];
                std::exception_ptr error{};
            };
            std::coroutine_handle<> await_by=std::noop_coroutine(); // caller
            _detail::coroutine_state state=_detail::awaiting;
        };
        using handle_type=std::coroutine_handle<promise_type>;
        handle_type coroutine;

        struct awaiter{
            handle_type coroutine;
            // never ready
            bool await_ready()const noexcept{
                return coroutine.done(); // always false
            }
            auto await_suspend(std::coroutine_handle<> handle)const noexcept{
                coroutine.promise().await_by=handle;
                return coroutine;
            }
            T await_resume()const{
                if(coroutine.promise().state==_detail::throws)
                    std::rethrow_exception(coroutine.promise().error);
                return std::move(reinterpret_cast<T&>(coroutine.promise().value));
            }
        };
        awaiter operator co_await ()const noexcept{
            return {coroutine};
        }
        ~async(){
            if(!coroutine.operator bool())[[unlikely]]return; // someone constructed empty object
            if(!coroutine.done()){ // free
                _detail::unowned_promise::adopt(std::move(*this));
            }else coroutine.destroy(); // co_awaited
        }

        // Move Only
        // When will C++ have static reflection

        async()=default;
        async(handle_type handle):coroutine(handle){}
        async(async&)=delete;
        async(async&&promise)noexcept{
            std::swap(coroutine,promise.coroutine);
        }
        async& operator=(async&)=delete;
        async& operator=(async&&promise)noexcept{
            std::swap(coroutine,promise.coroutine);
        }
    };

    template<>
    struct async<void>::promise_type{
        async<void> get_return_object(){return {handle_type::from_promise(*this)};}
        constexpr std::suspend_always initial_suspend()const noexcept{return {};}
        void return_void(){
            state=_detail::returned;
        }
        void unhandled_exception(){
            new(&error) std::exception_ptr(std::current_exception());
            state=_detail::throws;
        }
        struct suspend_final{
            constexpr bool await_ready()const noexcept{return false;}
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle)const noexcept{
                return handle.promise().await_by;
            }
            constexpr void await_resume()const noexcept{}
        };
        constexpr suspend_final final_suspend()const noexcept{
            return {};
        }
        ~promise_type(){
            if(state==_detail::throws) {
                error.~exception_ptr();
            }
        }
        std::exception_ptr error{};
        std::coroutine_handle<> await_by=std::noop_coroutine();
        _detail::coroutine_state state=_detail::awaiting;
    };

    template<>
    struct async<void>::awaiter{
        handle_type coroutine;
        bool await_ready()const noexcept{
            return coroutine.done();
        }
        auto await_suspend(std::coroutine_handle<> handle)const noexcept{
            coroutine.promise().await_by=handle;
            return coroutine;
        }
        void await_resume()const{
            if(coroutine.promise().state==_detail::throws)
                std::rethrow_exception(coroutine.promise().error);
        }
    };


    namespace _detail{
        template<typename T>
        struct notifier_slot{
            std::coroutine_handle<> coroutine;
            T *value=nullptr;
            bool await_ready()const noexcept{
                return false;
            }
            void await_suspend(std::coroutine_handle<> handle)noexcept{
                coroutine=handle;
            }
            T await_resume()const {
                if(value==nullptr)[[unlikely]]throw _detail::awaiting_notifier_destructed{};
                return *value;
            }
            void notify(T& t){
                value=&t;
                coroutine.resume();
            }
        };

        template<>
        struct notifier_slot<void>{
            std::coroutine_handle<> coroutine;
            void *value=nullptr;
            bool await_ready()const noexcept{
                return false;
            }
            void await_suspend(std::coroutine_handle<> handle)noexcept{
                coroutine=handle;
            }
            void await_resume()const{
                if(value==nullptr)[[unlikely]]throw _detail::awaiting_notifier_destructed{};
            }
            void notify(){
                value=reinterpret_cast<void *>(0xdedeaded);
                coroutine.resume();
            }
        };
    }

    template<typename T>
    struct notifier{
        std::vector<_detail::notifier_slot<T>> listener;
        _detail::notifier_slot<T>& operator co_await (){
            listener.push_back({});
            return listener.back();
        }
        void notify(T &t){
            decltype(listener) old;
            std::swap(old,listener);
            for(auto &a:old)
                a.notify(t);
        }
        void notify(T &&t){
            notify(t);
        }
        ~notifier(){
            for(auto &a:listener)
                a.coroutine.resume();
        }
    };
    template<>
    struct notifier<void>{
        std::vector<_detail::notifier_slot<void>> listener;
        _detail::notifier_slot<void>& operator co_await (){
            listener.push_back({});
            return listener.back();
        }
        void notify(){
            decltype(listener) old;
            std::swap(old,listener);
            for(auto &a:old)
                a.notify();
        }
        ~notifier(){
            for(auto &a:listener)
                a.coroutine.resume();
        }
    };

    template<typename T>
    struct co_returner{
        std::coroutine_handle<> handle;
        alignas(T) std::byte value[sizeof(T)];
        void return_value(T t){
            new (&value) T(std::move(t));
            return handle.resume();
        }
    };
    template<>
    struct co_returner<void>{
        std::coroutine_handle<> handle;
        void return_void() const{
            return handle.resume();
        }
    };
    namespace _detail{
        template<typename F,typename T>
        struct _task_execute_awaiter:public co_returner<T>{
            F func;
            static constexpr bool await_ready() noexcept{return false;}
            void await_suspend(std::coroutine_handle<> handle)noexcept{
                this->handle=handle;
                func(static_cast<co_returner<T>&>(*this));
            }
            T await_resume()noexcept{return std::move(reinterpret_cast<T&>(this->value));}
        };
        template<typename F>
        struct _task_execute_awaiter<F,void>:public co_returner<void>{
            F func;
            static constexpr bool await_ready() noexcept{return false;}
            void await_suspend(std::coroutine_handle<> handle)noexcept{
                this->handle=handle;
                func(static_cast<co_returner<void>&>(*this));
            }
            static void await_resume()noexcept{}
        };
    }

    template<typename T, std::invocable<co_returner<T>&> F>
    inline _detail::_task_execute_awaiter<F,T> do_task(F func){
        return {{},std::move(func)};
    }

}


#endif