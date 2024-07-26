#include <iostream>
#include "async.hpp"
using namespace std;
using namespace chzn;
notifier<string> readStr;
// 0: coroutine with chzn::async
async<string> getUpperStr(){
    auto str=co_await readStr;
    for(auto&c:str)if(c>='a'&&c<='z')c=c-'a'+'A';
    co_return str;
}
async<void> readChzn(){
    while(co_await readStr!="chzn");
    co_return;
}
int main(){
    // 1: chained call
    bool scanning=true;
    bool echoing=true;
    [&]()->async<void>{// echo uppercased string
        for(string str=co_await getUpperStr();echoing;str=co_await getUpperStr())
        cout<<str<<endl;
    }();
    [&]()->async<void>{// when cin "chzn" stop scanning
        co_await readChzn();
        scanning=false;
    }();
    // 2: notifier.notify
    while(scanning){
        string str;
        cin>>str;
        readStr.notify(str);
    }
    echoing=false;
    readStr.notify("one more");
    // 3: callback wrapper
    void (*callback)(string,void *userptr);
    void *userptr=nullptr;
    [&]()->async<void>{
        cout<< co_await do_async<string>(
            [&](chzn::co_returner<string>&r){
                callback=[](string str,void*ptr){
                    ((chzn::co_returner<string>*)ptr)->return_value(str);
                };
                userptr=&r;
            }
    );
        cout<<endl;
    }();

    callback("callback",userptr);

    // 4: cancelable task
    scanning=true;
    [&]()->async<void>{// when cin "chzn" stop scanning
        co_await readChzn();
        scanning=false;
    }();
    auto t=[&]()->task{
        for(string str=co_await readStr;;str=co_await readStr)
        cout<<str<<endl;
    }();
    t.run();
    while(scanning){
        string str;
        cin>>str;
        if(str=="echo")// when cin "echo" stop echoing
            t.cancel();
        readStr.notify(str);
    }
}
