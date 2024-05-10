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
        struct awaiting_notifier_destructed{};
        struct unowned_promise{
            struct promise_type{
                unowned_promise get_return_object(){return {handle_type::from_promise(*this)};}
                constexpr std::suspend_never initial_suspend()const noexcept{return {};}
                constexpr std::suspend_never final_suspend()const noexcept{return {};}
                static void unhandled_exception(){
                    throw;}
                constexpr void return_void()const noexcept{}
            };
            using handle_type=std::coroutine_handle<promise_type>;
            handle_type coroutine;

            template<typename T>
            static unowned_promise adopt(async<T> promise){
                try{
                    co_await promise;
                }catch (awaiting_notifier_destructed){}
            }
        };
    }
    template<typename T>
    struct async{
        struct promise_type{
            promise_type()noexcept :await_by(std::noop_coroutine()),state(_detail::awaiting){}
            async<T> get_return_object(){return {handle_type::from_promise(*this)};}
            constexpr std::suspend_never initial_suspend()const noexcept{return {};}
            void return_value(T t){
                state=_detail::returned;
                new(&value) T(std::move(t));
            }
            void unhandled_exception(){
                state=_detail::throws;
                new(&error) std::exception_ptr(std::current_exception());
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
                switch (state) {
                    case _detail::returned:
                        value.~T();
                        break;
                    case _detail::throws:
                        error.~exception_ptr();
                        break;
                }
            }
            std::coroutine_handle<> await_by;
            _detail::coroutine_state state;
            union {
                T value;
                std::exception_ptr error;
            };
        };
        using handle_type=std::coroutine_handle<promise_type>;
        handle_type coroutine;

        struct awaiter{
            const async &promise;
            bool await_ready()const noexcept{
                return promise.coroutine.done();
            }
            void await_suspend(std::coroutine_handle<> handle)const noexcept{
                promise.coroutine.promise().await_by=handle;
            }
            T await_resume()const{
                if(promise.coroutine.promise().state==_detail::throws)
                    std::rethrow_exception(promise.coroutine.promise().error);
                return std::move(promise.coroutine.promise().value);
            }
        };
        awaiter operator co_await ()const noexcept{
            return {*this};
        }
        ~async(){
            if(!coroutine.operator bool())[[unlikely]]return;
            if(!coroutine.done()){
                _detail::unowned_promise::adopt(std::move(*this));
            }else coroutine.destroy();
        }

        async()=default;
        async(handle_type handle):coroutine(handle){}
        async(async&)=delete;
        async(async&&promise)noexcept{
            std::swap(coroutine,promise.coroutine);
        }
        async& operator=(async&&promise)noexcept{
            std::swap(coroutine,promise.coroutine);
        }
    };

    template<>
    struct async<void>::promise_type{
        promise_type()noexcept :await_by(std::noop_coroutine()),state(_detail::awaiting){}
        async<void> get_return_object(){return {handle_type::from_promise(*this)};}
        constexpr std::suspend_never initial_suspend()const noexcept{return {};}
        void return_void(){
            state=_detail::returned;
        }
        void unhandled_exception(){
            state=_detail::throws;
            new(&error) std::exception_ptr(std::current_exception());
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
        std::coroutine_handle<> await_by;
        _detail::coroutine_state state;
        std::exception_ptr error;
    };

    template<>
    struct async<void>::awaiter{
        const async &promise;
        bool await_ready()const noexcept{
            return promise.coroutine.done();
        }
        void await_suspend(std::coroutine_handle<> handle)const noexcept{
            promise.coroutine.promise().await_by=handle;
        }
        void await_resume()const{
            if(promise.coroutine.promise().state==_detail::throws)
                std::rethrow_exception(promise.coroutine.promise().error);
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

}


#endif