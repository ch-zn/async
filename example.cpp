#include "async.hpp"
#include <iostream>
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
    [&]()->async<void>{
        while(1){
            auto str=co_await getUpperStr();
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