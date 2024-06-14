#include <iostream>
#include "async.hpp"
#include <unistd.h>
#include <thread>

using namespace chzn;
using namespace std;

int main(){
    notifier<string> notifier;
    auto getStr=[&]()->async<string>{
        co_return co_await notifier;
    };
    auto getUpperStr=[&]()->async<string>{
        auto str=co_await getStr();
        if(str=="throw")throw std::runtime_error(":P");
        for(auto &c:str)
            if(c>='a'&&c<='z')
                c+='A'-'a';
        co_return str;
    };
    auto delayStr=[&]()->async<std::string>{
        auto str=co_await getUpperStr();
        co_return co_await do_task<std::string>([=](auto&ret){
        std::thread([=,&ret](){
            usleep(1000000);
            ret.return_value(str+" delayed");
        }).detach();
        });
    };
    [&]()->async<>{
        while(1){
            auto str=co_await delayStr();
            cout<<str<<endl;
        }
    }();

    while(1){
        string str;
        cin>>str;
        try{
            notifier.notify(str);
        }catch (std::runtime_error&r){
            cerr<<r.what()<<"\n";
            return 0;
        }
    }
}