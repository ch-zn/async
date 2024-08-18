/*
 * file: async.hpp
 * author: chzn
 * license: zlib license
 * */
/*
  Copyright (C) 2024 chzn or chzn__

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
 */
#ifndef CHZN_UTIL_ASYNC_HPP
#define CHZN_UTIL_ASYNC_HPP

#include <coroutine>
#include <exception>
#include <any>
#include <functional>

/*
 * version 1.0.0 Everything Move Only
 * 2024/6/15
 * type:
 * - chzn::async<T>
 *   usage:
 *   - a function return chzn::async<T> is a coroutine, this coroutine co_return T;
 *   - can be co_awaited by another coroutine, will get a value type T;
 *   - lazy start, start when be co_awaited or when destruct;
 *   - move only;
 * - chzn::notifier<T>
 *   usage:
 *   - can be co_awaited by coroutine;
 *   - notify(t) will resume all coroutine co_awaiting this, they will get the value t;
 *   - when destruct, all coroutine co_awaiting this will get exception type chzn::no_longer_awaitable;
 *   - move only;
 *   - T is copyable;
 *   member function:
 *   - notify(T t) requires T!=void
 *     resume all coroutine co_awaiting this, they will get the value t;
 *   - notify()    requires T==void
 *     resume all coroutine co_awaiting this;
 * - chzn::no_longer_awaitable
 *   usage:
 *   - an exception thrown when co_awaiting a notifier and the notifier destruct;
 * - chzn::awaiter<T>
 *   usage:
 *   - type erased container of awaitable and moveable object;
 *   - when be co_awaited, will get a value type T;
 *   - move only;
 * - chzn::co_returner<T> and chzn::do_async<T>(F)
 *   usage:
 *   - tool to wrapper callback to coroutine;
 *   - do not copy/move, only co_await;
 *   - F is function type like void(chzn::co_returner<T>&);
 *   - void wrapperCallback(chzn::co_returner<int> &r){
 *         addCallback([&](int v){r.return_value(v);});
 *     }
 *     co_await chzn::do_async<T>(wrapperCallback);
 *   member function:
 *   - return_value(T t) requires T!=void
 *     resume coroutine co_awaiting this, it will get the value t;
 *   - return_void()     requires T==void
 *     resume coroutine co_awaiting this;
 *
 * version 1.0.1
 * 2024/7/28
 * changes:
 * - chzn::notifier now use custom list, because handle std::list::iterator only is not enough to erase element;
 * */
namespace chzn{
    template<typename T>
    struct async;
    namespace _detail{
        enum coroutine_state:unsigned char{
            awaiting,
            returned,
            throws,
        };
        struct awaiting_notifier_destructed{
        };// throw on co_await no longer return

        // helper class to release coroutine handle at correct time
        struct unowned_promise{
            struct promise_type{
                unowned_promise get_return_object(){return {handle_type::from_promise(*this)};}

                constexpr std::suspend_never initial_suspend() const noexcept{return {};}

                constexpr std::suspend_never final_suspend() const noexcept{return {};}

                static void unhandled_exception(){throw;}

                constexpr void return_void() const noexcept{}
            };

            using handle_type=std::coroutine_handle<promise_type>;
            handle_type coroutine;

            template<typename T>
            static unowned_promise adopt(T promise){
                try{
                    co_await promise;
                }catch(awaiting_notifier_destructed){}
            }

            template<typename T>
            static void take(T promise){
                auto &await_by=promise.coroutine.promise().await_by;
                await_by=[](T)->unowned_promise{co_await std::suspend_always{};}(std::move(promise)).coroutine;
            }
        };
    }

    using no_longer_awaitable=_detail::awaiting_notifier_destructed;

    template<typename T=void>
    struct async{
        struct promise_type{
            async<T> get_return_object(){return {handle_type::from_promise(*this)};}

            // suspend at start to make caller co_await this, then set await_by when this be co_await
            constexpr std::suspend_always initial_suspend() const noexcept{return {};}

            void return_value(T t){
                new(&value) T(std::move(t));
                state=_detail::returned;
            }

            void unhandled_exception(){
                new(&error) std::exception_ptr(std::current_exception());
                state=_detail::throws;
            }

            struct suspend_final:public std::suspend_always{
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) const noexcept{
                    return handle.promise().await_by;
                }
            };

            // suspend at final, then resume the caller
            constexpr suspend_final final_suspend() const noexcept{
                return {};
            }

            ~promise_type(){
                switch(state){
                    case _detail::returned:
                        reinterpret_cast<T &>(value).~T();
                        break;
                    case _detail::throws:
                        error.~exception_ptr();
                        break;
                }
            }

            union{
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
            bool await_ready() const noexcept{
                return coroutine.done(); // always false
            }

            auto await_suspend(std::coroutine_handle<> handle) const noexcept{
                coroutine.promise().await_by=handle;
                return coroutine;
            }

            T await_resume() const{
                if(coroutine.promise().state==_detail::throws)
                    std::rethrow_exception(coroutine.promise().error);
                return std::move(reinterpret_cast<T &>(coroutine.promise().value));
            }
        };

        awaiter operator
        co_await ()const noexcept{
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

        async() = default;

        async(handle_type handle):coroutine(handle){}

        async(async &) = delete;

        async(async &&promise) noexcept{
            std::swap(coroutine,promise.coroutine);
        }

        async &operator=(async &) = delete;

        async &operator=(async &&promise) noexcept{
            std::swap(coroutine,promise.coroutine);
            return *this;
        }
    };

    template<>
    struct async<void>::promise_type{
        async<void> get_return_object(){return {handle_type::from_promise(*this)};}

        constexpr std::suspend_always initial_suspend() const noexcept{return {};}

        void return_void(){
            state=_detail::returned;
        }

        void unhandled_exception(){
            new(&error) std::exception_ptr(std::current_exception());
            state=_detail::throws;
        }

        struct suspend_final{
            constexpr bool await_ready() const noexcept{return false;}

            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> handle) const noexcept{
                return handle.promise().await_by;
            }

            constexpr void await_resume() const noexcept{}
        };

        constexpr suspend_final final_suspend() const noexcept{
            return {};
        }

        ~promise_type(){
            if(state==_detail::throws){
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

        bool await_ready() const noexcept{
            return coroutine.done();
        }

        auto await_suspend(std::coroutine_handle<> handle) const noexcept{
            coroutine.promise().await_by=handle;
            return coroutine;
        }

        void await_resume() const{
            if(coroutine.promise().state==_detail::throws)
                std::rethrow_exception(coroutine.promise().error);
        }
    };

    namespace _detail{
        template<typename T>
        struct _awaiter_operator{
            bool (*ready)(std::any &);

            void (*suspend)(std::any &,std::coroutine_handle<>);

            T (*resume)(std::any &);
        };

        template<typename U>
        struct _awaiter_operator_helper{
            static _awaiter_operator<std::invoke_result_t<decltype(&U::await_resume),U &>> &func(){
                static _awaiter_operator<std::invoke_result_t<decltype(&U::await_resume),U &>> op={
                        .ready=[](std::any &self){
                            return std::any_cast<U &>(self).await_ready();
                        },
                        .suspend=[](std::any &self,std::coroutine_handle<> handle){
                            std::any_cast<U &>(self).await_suspend(handle);
                        },
                        .resume=[](std::any &self){
                            return std::any_cast<U &>(self).await_resume();
                        }
                };
                return op;
            }
        };
    }

    template<typename T>
    struct awaiter{
        std::any ptr;
        _detail::_awaiter_operator<T> *op;

        template<typename U>
        requires std::invocable<decltype(&U::await_ready),U &>
                 &&std::invocable<decltype(&U::await_suspend),U &,std::coroutine_handle<>>
                 &&std::invocable<decltype(&U::await_resume),U &>
        awaiter(U t):op(&_detail::_awaiter_operator_helper<U>::func()),ptr(std::move(t)){}

        bool await_ready(){return op->ready(ptr);}

        void await_suspend(std::coroutine_handle<> handle){op->suspend(ptr,handle);}

        T await_resume(){return op->resume(ptr);}

        awaiter(awaiter &) = delete;

        awaiter(awaiter &&a) noexcept{
            std::swap(ptr,a.ptr);
            std::swap(op,a.op);
        }

        void operator=(awaiter &) = delete;

        awaiter &operator=(awaiter &&a) noexcept{
            std::swap(ptr,a.ptr);
            std::swap(op,a.op);
            return *this;
        }
    };

    template<typename T> requires std::invocable<decltype(&T::await_ready),T &>
                                  &&std::invocable<decltype(&T::await_suspend),T &,std::coroutine_handle<>>
                                  &&std::invocable<decltype(&T::await_resume),T &>
    awaiter(T t)->awaiter<std::invoke_result_t<decltype(&T::await_resume),T &>>;

    namespace _detail{
        template<typename T>
        struct notifier_slot{
            std::coroutine_handle<> coroutine;
            T *value=nullptr;

            bool await_ready() const noexcept{
                return false;
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept{
                coroutine=handle;
            }

            T await_resume() const{
                if(value==nullptr)[[unlikely]]throw _detail::awaiting_notifier_destructed{};
                return *value;
            }

            void notify(T &t){
                value=&t;
                coroutine.resume();
            }

            void notify(T &&t){
                value=&t;
                coroutine.resume();
            }
        };

        template<>
        struct notifier_slot<void>{
            std::coroutine_handle<> coroutine;
            void *value=nullptr;

            bool await_ready() const noexcept{
                return false;
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept{
                coroutine=handle;
            }

            void await_resume() const{
                if(value==nullptr)[[unlikely]]throw _detail::awaiting_notifier_destructed{};
            }

            void notify(){
                value=reinterpret_cast<void *>(0xdedeaded);
                coroutine.resume();
            }
        };

        template<typename T>
        struct _notifier_slot_list{
            struct node{
                node *last,*next;notifier_slot<T> value;
                // stl::list don't contain this, because it preserve size;
                void erase(){
                    last->next=next;
                    next->last=last;
                    delete this;
                }
            };
            struct iterator{
                node *n;
                notifier_slot<T>& operator*(){return n->value;}
                void operator++(){n=n->next;}
                bool operator==(iterator b)const noexcept{return n==b.n;}
            };
            node *the_end;

            _notifier_slot_list():the_end(new node){the_end->last=the_end->next=the_end;}
            iterator begin(){return {the_end->next};}
            iterator end(){return {the_end};}
            notifier_slot<T>& push(){
                auto new_node=new node{the_end->last,the_end,{}};
                the_end->last->next=new_node;
                the_end->last=new_node;
                return new_node->value;
            }
            node* last_push(){
                return the_end->last;
            }
            ~_notifier_slot_list(){
                for(auto it=the_end->next,nit=it->next;it!=the_end;it=nit,nit=it->next){
                    delete it;
                }
                delete the_end;
            }
            _notifier_slot_list(_notifier_slot_list&)=delete;
        };

        template<typename T>
        void swap(_notifier_slot_list<T>&a,_notifier_slot_list<T>&b){
            std::swap(a.the_end,b.the_end);
        }
    }

    template<typename T>
    struct notifier{
        // list keep reference
        _detail::_notifier_slot_list<T> listener;
        _detail::notifier_slot<T> &operator
        co_await (){
            return listener.push();
        }

        void notify(T &t){
            decltype(listener) old;
            swap(old,listener);
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

        notifier() = default;

        notifier(notifier &) = delete;

        notifier(notifier &&n) noexcept{swap(listener,n.listener);}

        void operator=(notifier &) = delete;

        notifier &operator=(notifier &&t) noexcept{
            swap(listener,t.listener);
            return *this;
        }
    };

    template<>
    struct notifier<void>{
        _detail::_notifier_slot_list<void> listener;
        _detail::notifier_slot<void> &operator
        co_await (){
            return listener.push();
        }

        void notify(){
            decltype(listener) old;
            swap(old,listener);
            for(auto &a:old)
                a.notify();
        }

        ~notifier(){
            for(auto &a:listener)
                a.coroutine.resume();
        }

        notifier() = default;

        notifier(notifier &) = delete;

        notifier(notifier &&n) noexcept{swap(listener,n.listener);}

        void operator=(notifier &) = delete;

        notifier &operator=(notifier &&n) noexcept{
            swap(listener,n.listener);
            return *this;
        }
    };

    template<typename T>
    struct co_returner{
        std::coroutine_handle<> handle;
        alignas(T) std::byte value[sizeof(T)];

        void return_value(T t){
            new(&value) T(std::move(t));
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

            void await_suspend(std::coroutine_handle<> handle) noexcept{
                this->handle=handle;
                func(static_cast<co_returner<T> &>(*this));
            }

            T await_resume() noexcept{return std::move(reinterpret_cast<T &>(this->value));}
        };

        template<typename F>
        struct _task_execute_awaiter<F,void>:public co_returner<void>{
            F func;

            static constexpr bool await_ready() noexcept{return false;}

            void await_suspend(std::coroutine_handle<> handle) noexcept{
                this->handle=handle;
                func(static_cast<co_returner<void> &>(*this));
            }

            static void await_resume() noexcept{}
        };
    }

    template<typename T,std::invocable<co_returner<T> &> F>
    inline _detail::_task_execute_awaiter<F,T> do_async(F func){
        return {{},std::move(func)};
    }

}

/*
 * version 1.1.0 Cancelable Task
 * 2024/7/26
 * type:
 * - chzn::task
 *   usage:
 *   - cancel() to cancel cancelable coroutine;
 *   - not co_awaitable;
 *   - move only;
 *   member function:
 *   - cancel()
 *     cancel cancelable coroutine;
 *
 * version 1.1.1
 * 2024/7/28
 * changes:
 * - task is no longer lazy start, because it can't be co_awaited;
 * - task co_await notifier needn't create proxy layer;
 *
 * version 1.1.2
 * 2024/8/17
 * changes:
 * - a running task (not suspended, still in stack) no longer cancelable
 *   try to cancel it will throw chzn::cancel_running_task_error
 *   if you want cancel task in itself, use:
 *       co_await [&](this auto)->chzn::async<void>{co_return task.cancel();}();
 *       (use 'deducing this' to avoid dangling reference 'this' of lambda)
 * */
#include <stdexcept>
namespace chzn{
    struct task;
    namespace _detail{
        template<typename T,bool keep>
        struct _task_transformed_async{
            struct promise_type:public async<T>::promise_type{
                _task_transformed_async<T,keep> get_return_object(){return {handle_type::from_promise(*this)};}

                struct suspend_final:public std::suspend_always{
                    std::coroutine_handle<> await_suspend(
                            std::coroutine_handle<promise_type> handle) const noexcept{return handle.promise().await_by;}
                };

                constexpr suspend_final final_suspend() const noexcept{return {};}

                template<typename U>
                promise_type(U &u,void(*&cancel_func_ref)(void*)){cancel_func_ptr=&cancel_func_ref;}
                void(**cancel_func_ptr)(void*);
            };

            using handle_type=std::coroutine_handle<promise_type>;
            handle_type coroutine=nullptr;

            struct awaiter{
                handle_type coroutine;

                bool await_ready() const noexcept{return coroutine.done();}

                auto await_suspend(std::coroutine_handle<> handle) const noexcept{
                    coroutine.promise().await_by=handle;
                    return coroutine;
                }

                T await_resume() const{
                    if(coroutine.promise().state==_detail::throws)
                        std::rethrow_exception(coroutine.promise().error);
                    if constexpr(!std::is_same_v<T,void>)return std::move(reinterpret_cast<T &>(coroutine.promise().value));
                }
            };

            awaiter operator
            co_await()const noexcept{return {coroutine};}

            _task_transformed_async(_task_transformed_async &) = delete;

            _task_transformed_async(_task_transformed_async &&t){std::swap(coroutine,t.coroutine);}

            void operator=(_task_transformed_async) = delete;

            ~_task_transformed_async(){
                if(coroutine){
                    if(coroutine.done()&&coroutine.promise().cancel_func_ptr)
                        *coroutine.promise().cancel_func_ptr=nullptr;
                    if(!keep||coroutine.done())coroutine.destroy();
                    else _detail::unowned_promise::take(std::move(*this));
                }
            }

        private:
            _task_transformed_async(handle_type handle):coroutine(handle){}

            friend promise_type;
            friend task;

            template<typename U>
            static _task_transformed_async<T,keep> transform(U u,void(*&cancel_func)(void*)){
                co_return co_await u;
            }

            template<typename U>
            static _task_transformed_async<T,keep> transform(_detail::notifier_slot<U> &u,void(*&cancel_func)(void*)){
                co_return co_await u;
            }
        };

        template<typename T>
        struct _co_awaiter_T{
            using type=decltype(std::declval<T>().await_resume());
        };

        template<typename T>
        struct _co_await_T{
            using type=_co_awaiter_T<T>::type;
        };

        template<typename T>
        concept _have_co_await_member_function = requires(T t) {
            t.operator co_await ();
        };

        template<typename T>
        concept _have_co_await_global_function = requires(T t) {
            operator co_await (t);
        };

        template<_have_co_await_member_function T>
        struct _co_await_T<T>{
            using type=_co_awaiter_T<decltype(std::declval<T>().operator co_await())>::type;
        };

        template<_have_co_await_global_function T>
        struct _co_await_T<T>{
            using type=_co_awaiter_T<decltype(operator co_await(

            std::declval<T>()

            ))>::type;
        };

        inline void noop_cancel_func(void*){}
    }
    struct cancel_running_task_error:std::runtime_error{
        cancel_running_task_error():std::runtime_error("chzn::task be canceled when running (in stack, not suspend)"){}
    };
    struct task{
        struct promise_type:public async<void>::promise_type{
            task get_return_object(){return {handle_type::from_promise(*this)};}

            constexpr std::suspend_never initial_suspend()const noexcept{return {};}

            constexpr std::suspend_always final_suspend() const noexcept{return {};}

            void(*cancel_func)(void*)=nullptr;
            void* cancel_token=nullptr;

            template<typename U>
            auto await_transform(U &&u){
                auto t=_detail::_task_transformed_async<typename _detail::_co_await_T<U>::type,true>::transform(std::forward<U>(u),cancel_func);
                cancel_token=&t.coroutine.promise();
                cancel_func=[](void *token){(decltype(&t.coroutine.promise())(token))->await_by=std::noop_coroutine();(decltype(&t.coroutine.promise())(token))->cancel_func_ptr=nullptr;};
                return t;
            }

            template<typename U>
            auto await_transform(notifier<U> &u){
                auto &t=u.operator co_await();
                cancel_token=u.listener.last_push();
                cancel_func=[](void *token){((decltype(u.listener.last_push()))token)->erase();};
                return _detail::_task_transformed_async<typename _detail::_co_await_T<decltype(t)>::type,false>::transform(t,cancel_func);
            }

            std::suspend_always await_transform(std::suspend_always){cancel_func=_detail::noop_cancel_func;return {};}
        };

        using handle_type=std::coroutine_handle<promise_type>;
        handle_type coroutine;

        void cancel(){
            auto &t=coroutine.promise().cancel_func;
            if(!t)[[unlikely]]throw cancel_running_task_error();
            t(coroutine.promise().cancel_token);
            t=_detail::noop_cancel_func;
        }

        ~task(){
            if(!coroutine.operator bool())[[unlikely]]return;
            if(!coroutine.done())
                cancel();
            coroutine.destroy();
        }

        task() = default;

        task(handle_type handle):coroutine(handle){}

        task(task &) = delete;

        task(task &&t) noexcept{std::swap(coroutine,t.coroutine);}

        task &operator=(task t){
            std::swap(coroutine,t.coroutine);
            return *this;
        }
    };
}

#endif