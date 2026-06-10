#include<iostream>
#include<string>
#include<regex>

int main()
{
    //HTTP请求格式： GET /kiana/login?user=xiaoming&pass=123123 HTTP/1.1\r\n
    std::string str = "GET /kiana/login?user=xiaoming&pass=123123 HTTP/1.1";

    std::smatch matches;
    //请求方法的匹配 GET HEAD POST PUT DELETE...
    //std::regex e("(GET|HEAD|POST|PUT|DELETE) .*");
    //std::regex e("(GET|HEAD|POST|PUT|DELETE) ([^?]*).*");
    //GET|HEAD|POST|PUT|DELETE 表示匹配并提取其中任意一个字符串
    //[^?]*  [^*]匹配非问号字符，后边的*表示0次或多次
    //std::regex e("(GET|HEAD|POST|PUT|DELETE) ([^?]*)\\?(.*) .*");
    //std::regex e("(GET|HEAD|POST|PUT|DELETE) ([^?]*)\\?(.*) (HTTP/1\\.[01])(?:\n|\r\n)?");
    //(?:\n|\r\n)?   (?: ...)表示匹配某个格式字段串，但是不提取。 最后的？表示的是匹配前边的表达式0次或1次
    std::regex e("(GET|HEAD|POST|PUT|DELETE) ([^?]*)(?:\\?(.*))? (HTTP/1\\.[01])(?:\n|\r\n)?");

    bool ret = std::regex_match(str,matches,e);
    if(ret == false){
        return -1;
    }

    for(auto &s:matches)
    {
        std::cout<<s<<std::endl;
    }
    return 0;
}