#include "library.h"
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
namespace {
bool is_dir(const std::string&p){struct stat s{};return stat(p.c_str(),&s)==0&&S_ISDIR(s.st_mode);}
std::string trim(std::string s){auto n=[](unsigned char c){return !std::isspace(c);};s.erase(s.begin(),std::find_if(s.begin(),s.end(),n));s.erase(std::find_if(s.rbegin(),s.rend(),n).base(),s.end());return s;}
void load_ini(GameEntry&g){std::ifstream f(g.path+"/game.ini");std::string l;while(std::getline(f,l)){l=trim(l);if(l.empty()||l[0]=='#'||l[0]==';')continue;auto e=l.find('=');if(e==std::string::npos)continue;auto k=trim(l.substr(0,e)),v=trim(l.substr(e+1));if(k=="name"&&!v.empty())g.name=v;else if(k=="backend"&&!v.empty())g.backend=v;else if(k=="entry"&&!v.empty())g.entry=v;}}
}
std::vector<GameEntry> scan_game_library(const std::string&root){std::vector<GameEntry>v;DIR*d=opendir(root.c_str());if(!d)return v;while(auto*i=readdir(d)){std::string n=i->d_name;if(n=="."||n=="..")continue;std::string p=root+"/"+n;if(!is_dir(p))continue;GameEntry g{n,p,"auto",""};load_ini(g);v.push_back(g);}closedir(d);std::sort(v.begin(),v.end(),[](const auto&a,const auto&b){return a.name<b.name;});return v;}
