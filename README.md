# admincmd
Linux deamons/servers often need a back door for administration purposes. This project provides a solution that help build a command line server that can be easily be incorporated into an existing program written in c++, and users can use tools such as telnet or nc to communicate with the server.

The main work user need to do is to include one header file `AdminCMDServer.h`, create an object of `AdminCMDServer`, provide it with server ip and port to listen on, and implement 3 callback functions in a CRTP-ish style to handle events:

```c++
// A client established a connection to the server, return an optional welcome msg
std::string onAdminConnect(AdminCMDServer::Connection& conn);

// A client disconnected from the server
void onAdminDisconnect(AdminCMDServer::Connection& conn, const std::string& error);

// A client sent a command, return an optional response msg
std::string onAdminCMD(AdminCMDServer::Connection& conn, int argc, const char** argv);
```
And each admin connection has below interface for user to operate with:

```c++
class Connection {
public:
  // data member with user defined type
  ConnUserData user_data;
  
  // get remote network address
  bool getPeername(struct sockaddr_in& addr);
  
  // close this connection with a reason
  void close(const char* reason = "user close");
};
```

## Thread Model
AdminCMDServer does not create threads itself, thus it needs user to drive it by calling `poll()` repetitively. It's up to the user to use existing threads or create an new thread to poll AdminCMDServer, and it's user's responsibility to ensure thread safety.

## Server Output
AdminCMDServer does not write errors to stdout or log files. User should use `getLastError()` to get error msg when `init()` failed.

## Connection Timeout
By default admin connection won't timeout, but it can be enabled by setting AdminCMDServer's template parameter `uint64_t ConnTimeout`,
the value is measured by a difference of user provided timestamps, and the timestamps are passed in as the optional parameter of poll: `void poll(uint64_t now = 0)`.

## Command Format
Shell style command is supported, that is, when you build a c program and run it by `./a.out args...`, the arguments will be parsed and provided by `int argc, const char** argv` in the program. To be specific, when not in quotes, all characters will be escaped by backslash(`\`), and spaces will delimite arguments and line feed(`\n`) will end the command; When in single quotes(`'`), none character will be escaped; When in double quotes(`"`), only backslash(`\`) and double quote(`"`) will be escaped.

## References
https://github.com/MengRao/pollnet
