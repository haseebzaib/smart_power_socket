#include "sim7080.hpp"
#include <cerrno>
#include <charconv>
#include <cstdio>
namespace {
bool number(std::string_view &text, uint32_t &out, bool last)
{
    while (!text.empty() && text.front()==' ') text.remove_prefix(1);
    auto r=std::from_chars(text.data(),text.data()+text.size(),out);
    if (r.ec!=std::errc{}) return false;
    text.remove_prefix(r.ptr-text.data());
    while (!text.empty() && (text.front()==' ' || text.front()=='\r' || text.front()=='\n')) text.remove_prefix(1);
    if (last) return text.empty();
    if (text.empty() || text.front()!=',') return false;
    text.remove_prefix(1);
    return true;
}
bool safe_file(std::string_view file) {
    if(file.empty() || file.size()>32) return false;
    for(char c:file) if (!(c>='a'&&c<='z') && !(c>='0'&&c<='9') && c!='-' && c!='.') return false;
    return true;
}
}
namespace cellular {
bool sim7080::http_notification(std::string_view line, void *self)
{
    if (!line.starts_with("+HTTPTOFS:")) return false;
    auto values=line.substr(10);
    uint32_t status,bytes;
    if (!number(values,status,false) || !number(values,bytes,true)) return false;
    auto &modem=*static_cast<sim7080 *>(self);
    if (modem.httpActive_) {
        modem.httpResult_.code=status;
        modem.httpResult_.bytes=bytes;
        modem.httpResult_.finished=true;
    }
    return true;
}
int sim7080::https_prepare(std::string_view host)
{
    if (host.empty() || host.size()>100) return -EINVAL;
    for (char c : host) if (!(c>='a'&&c<='z') && !(c>='A'&&c<='Z') &&
        !(c>='0'&&c<='9') && c!='-' && c!='.') return -EINVAL;
    if (modemInformation_.networkRegistration!=1 && modemInformation_.networkRegistration!=5) return -ENETDOWN;
    ensure_data_connection();
    if (!modemInformation_.dataConnected) return -ENETUNREACH;
    if (atEngine_.send_command("AT+CSSLCFG=\"SSLVERSION\",0,3\r\n",5000)!=atEngine::atResult::OK) return -EIO;
    std::array<char,160> command{};
    int n=std::snprintf(command.data(),command.size(),"AT+CSSLCFG=\"SNI\",0,\"%.*s\"\r\n",static_cast<int>(host.size()),host.data());
    if(n<0 || static_cast<size_t>(n)>=command.size()) return -EINVAL;
    return atEngine_.send_command({command.data(),static_cast<size_t>(n)},5000)==atEngine::atResult::OK ? 0 : -EIO;
}
int sim7080::file_open()
{
    if (fileOpen_) return 0;
    if (atEngine_.send_command("AT+CFSINIT\r\n",5000)!=atEngine::atResult::OK) return -EIO;
    fileOpen_=true;
    return 0;
}
int sim7080::file_close()
{
    if (!fileOpen_) return 0;
    auto r=atEngine_.send_command("AT+CFSTERM\r\n",5000);
    if (r!=atEngine::atResult::OK) return -EIO;
    fileOpen_=false;
    return 0;
}
int sim7080::file_size(std::string_view file,uint32_t &size)
{
    if (!fileOpen_ || !safe_file(file)) return -EINVAL;
    std::array<char,96> command{}, response{};
    int n=std::snprintf(command.data(),command.size(),"AT+CFSGFIS=0,\"%.*s\"\r\n",static_cast<int>(file.size()),file.data());
    auto r=atEngine_.send_command({command.data(),static_cast<size_t>(n)},"+CFSGFIS:",
        {reinterpret_cast<uint8_t *>(response.data()),response.size()},5000);
    if (r.result!=atEngine::atResult::OK) return -EIO;
    std::string_view value{response.data(),r.responseLength};
    if (!value.starts_with("+CFSGFIS:")) return -EBADMSG;
    value.remove_prefix(9);
    return number(value,size,true) ? 0 : -EBADMSG;
}
int sim7080::file_read(std::string_view file,uint32_t offset,std::span<uint8_t> output)
{
    if (!fileOpen_ || !safe_file(file) || output.empty() || output.size()>1024) return -EINVAL;
    std::array<char,128> command{};
    int n=std::snprintf(command.data(),command.size(),"AT+CFSRFILE=0,\"%.*s\",1,%u,%u\r\n",
        static_cast<int>(file.size()),file.data(),static_cast<unsigned>(output.size()),static_cast<unsigned>(offset));
    auto r=atEngine_.send_binary_command({command.data(),static_cast<size_t>(n)},"+CFSRFILE:",output,5000);
    return r.result==atEngine::atResult::OK && r.responseLength==output.size() ? 0 : -EIO;
}
int sim7080::https_start(std::string_view url,std::string_view file)
{
    if (httpActive_ || !safe_file(file) || !url.starts_with("https://") || url.size()>255) return -EINVAL;
    for (unsigned char c : url) if (c<=32 || c>=127 || c=='"' || c=='\\') return -EINVAL;
    int rc=file_open(); if (rc) return rc;
    uint32_t oldSize;
    if (!file_size(file,oldSize)) {
        std::array<char,96> remove{};
        int n=std::snprintf(remove.data(),remove.size(),"AT+CFSDFILE=0,\"%.*s\"\r\n",static_cast<int>(file.size()),file.data());
        if(atEngine_.send_command({remove.data(),static_cast<size_t>(n)},5000)!=atEngine::atResult::OK) return -EIO;
    }
    rc=file_close(); if (rc) return rc;
    std::array<char,400> command{};
    int n=std::snprintf(command.data(),command.size(),"AT+HTTPTOFS=\"%.*s\",\"/custapp/%.*s\",1000\r\n",
        static_cast<int>(url.size()),url.data(),static_cast<int>(file.size()),file.data());
    if (n<0 || static_cast<size_t>(n)>=command.size()) return -ENAMETOOLONG;
    httpResult_={};
    httpActive_=true; // Catch completion even when it precedes the command OK.
    if (atEngine_.send_command({command.data(),static_cast<size_t>(n)},5000)!=atEngine::atResult::OK) {
        httpActive_=false; return -EIO;
    }
    return 0;
}
int sim7080::https_poll(http_result &result)
{
    atEngine_.processRx();
    if (!httpActive_) return -EINVAL;
    if (httpResult_.finished) {
        result=httpResult_;
        httpActive_=false;
        return 0;
    }
    std::array<char,128> response{};
    auto r=atEngine_.send_command("AT+HTTPTOFSRL?\r\n","+HTTPTOFS",
        {reinterpret_cast<uint8_t *>(response.data()),response.size()},5000);
    if (r.result!=atEngine::atResult::OK) return -EIO;
    if (httpResult_.finished) {
        result=httpResult_; httpActive_=false; return 0;
    }
    std::string_view value{response.data(),r.responseLength};
    auto colon=value.find(':');
    if(colon==value.npos) return -EBADMSG;
    value.remove_prefix(colon+1);
    uint32_t state,received,total;
    if(!number(value,state,false)||!number(value,received,false)||!number(value,total,true)||state>1) return -EBADMSG;
    httpResult_.received=received; httpResult_.total=total;
    result=httpResult_;
    // Idle is not success: require the final HTTP status URC as well.
    return 0;
}
void sim7080::https_cleanup(bool cancel)
{
    (void)file_close();
    if (cancel && httpActive_) {
        atEngine_.send_command("AT+CNACT=0,0\r\n",5000);
        modemInformation_.dataConnected=0;
        nextDataConnectionRetryMs_=k_uptime_get()+dataConnectionRetryIntervalMs;
    }
    httpActive_=false;
    httpResult_={};
}
}
