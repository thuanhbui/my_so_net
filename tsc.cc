#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <csignal>
#include <grpc++/grpc++.h>
#include "client.h"
#include <regex>

#include "sns.grpc.pb.h"
using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using csce662::Message;
using csce662::ListReply;
using csce662::Request;
using csce662::Reply;
using csce662::SNSService;

void sig_ignore(int sig) {
  std::cout << "Signal caught " + sig;
}

Message MakeMessage(const std::string& username, const std::string& msg) {
    Message m;
    m.set_username(username);
    m.set_msg(msg);
    google::protobuf::Timestamp* timestamp = new google::protobuf::Timestamp();
    timestamp->set_seconds(time(NULL));
    timestamp->set_nanos(0);
    m.set_allocated_timestamp(timestamp);
    return m;
}

std::vector<std::string> split_string(const std::string& input, const std::string& delimiter) {
    std::vector<std::string> tokens;
    std::regex re(delimiter);
    std::sregex_token_iterator begin(input.begin(), input.end(), re, -1);
    std::sregex_token_iterator end;

    for (std::sregex_token_iterator i = begin; i != end; i++) {
	tokens.push_back(i->str());
    }

    return tokens;
}

class Client : public IClient
{
public:
  Client(const std::string& hname,
	 const std::string& uname,
	 const std::string& p)
    :hostname(hname), username(uname), port(p) {}

  
protected:
  virtual int connectTo();
  virtual IReply processCommand(std::string& input);
  virtual void processTimeline();

private:
  std::string hostname;
  std::string username;
  std::string port;
  
  // You can have an instance of the client stub
  // as a member variable.
  std::unique_ptr<SNSService::Stub> stub_;
  
  IReply Login();
  IReply List();
  IReply Follow(const std::string &username);
  IReply UnFollow(const std::string &username);
  void   Timeline(const std::string &username);
};


///////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////
int Client::connectTo()
{
  // ------------------------------------------------------------
  // In this function, you are supposed to create a stub so that
  // you call service methods in the processCommand/porcessTimeline
  // functions. That is, the stub should be accessible when you want
  // to call any service methods in those functions.
  // Please refer to gRpc tutorial how to create a stub.
  // ------------------------------------------------------------
    
///////////////////////////////////////////////////////////
// YOUR CODE HERE
//////////////////////////////////////////////////////////

    auto channel = grpc::CreateChannel("127.0.0.1:3010", grpc::InsecureChannelCredentials());
    stub_ = std::unique_ptr<SNSService::Stub>(SNSService::NewStub(channel));

    IReply ire = Login();
    return 1;
}

IReply Client::processCommand(std::string& input)
{
  // ------------------------------------------------------------
  // GUIDE 1:
  // In this function, you are supposed to parse the given input
  // command and create your own message so that you call an 
  // appropriate service method. The input command will be one
  // of the followings:
  //
  // FOLLOW <username>
  // UNFOLLOW <username>
  // LIST
  // TIMELINE
  // ------------------------------------------------------------
  
  // ------------------------------------------------------------
  // GUIDE 2:
  // Then, you should create a variable of IReply structure
  // provided by the client.h and initialize it according to
  // the result. Finally you can finish this function by returning
  // the IReply.
  // ------------------------------------------------------------
  
  
  // ------------------------------------------------------------
  // HINT: How to set the IReply?
  // Suppose you have "FOLLOW" service method for FOLLOW command,
  // IReply can be set as follow:
  // 
  //     // some codes for creating/initializing parameters for
  //     // service method
  //     IReply ire;
  //     grpc::Status status = stub_->FOLLOW(&context, /* some parameters */);
  //     ire.grpc_status = status;
  //     if (status.ok()) {
  //         ire.comm_status = SUCCESS;
  //     } else {
  //         ire.comm_status = FAILURE_NOT_EXISTS;
  //     }
  //      
  //      return ire;
  // 
  // IMPORTANT: 
  // For the command "LIST", you should set both "all_users" and 
  // "following_users" member variable of IReply.
  // ------------------------------------------------------------
    
    IReply ire;
    std::cout << "Receiving: " << input << std::endl;
    std::vector<std::string> tokens = split_string(input, " ");

    if (tokens.size() == 0) return ire;

    std::string cmd = tokens[0];

    if (cmd == "FOLLOW") {
      std::string follow_username = tokens[1];
      return Follow(follow_username);
    } else if (cmd == "UNFOLLOW") {
	    std::string unfollow_username = tokens[1];
	    return UnFollow(unfollow_username);
    }

    return ire;
}


void Client::processTimeline()
{
    Timeline(username);
}

// List Command
IReply Client::List() {

    IReply ire;

    /*********
    YOUR CODE HERE
    **********/

    return ire;
}

// Follow Command        
IReply Client::Follow(const std::string& username2) {    
 
    IReply ire;
    ClientContext context;
    Request request;
    Reply reply;

    request.set_username(username);
    request.add_arguments(username2);

    Status status = stub_->Follow(&context, request, &reply);
    ire.grpc_status = status;
    if (status.ok()) {
	    std::cout<<"Server message: "<<reply.msg() <<std::endl;
    	if (reply.msg() == "Invalid username: You cannot follow yourself!") {
	    ire.comm_status = FAILURE_INVALID_USERNAME;
    	} else if (reply.msg() == "Invalid username: User not found.") {
	    ire.comm_status = FAILURE_NOT_EXISTS;
    	} else if (reply.msg() == "You already followed this user!") {
	    ire.comm_status = FAILURE_ALREADY_EXISTS;
    	} else if (reply.msg() == "Follow successfully!") {
	    ire.comm_status = SUCCESS;
    	} else {
	    ire.comm_status = FAILURE_UNKNOWN;
	}
    }

    return ire;
}

// UNFollow Command  
IReply Client::UnFollow(const std::string& username2) {

    IReply ire;
    ClientContext context;
    Request request;
    Reply reply;

    request.set_username(username);
    request.add_arguments(username2);

    Status status = stub_->UnFollow(&context, request, &reply);
    ire.grpc_status = status;

    if (status.ok()) {
	    std::cout<<"Server message: " << reply.msg() <<std::endl;
	    if (reply.msg() == "Invalid username: You cannot unfollow yourself!") {
		    ire.comm_status = FAILURE_INVALID_USERNAME;
	    } else if (reply.msg() == "Invalid username: User not found.") {
		    ire.comm_status = FAILURE_NOT_EXISTS;
	    } else if (reply.msg() == "You are not a follower") {
		    ire.comm_status = FAILURE_NOT_A_FOLLOWER;
	    } else if (reply.msg() == "UnFollow sucessfully!") {
		    ire.comm_status = SUCCESS;
	    } else {
		    ire.comm_status = FAILURE_UNKNOWN;
	    }
    }

    return ire;
}

// Login Command  
IReply Client::Login() {

    ClientContext context;
    Request request;
    Reply reply;

    request.set_username(username);

    Status status = stub_->Login(&context, request, &reply);
    IReply ire;
    ire.grpc_status = status;
    ire.comm_status = SUCCESS;
    std::cout<<reply.msg()<<std::endl;

    return ire;
}

// Timeline Command
void Client::Timeline(const std::string& username) {

    // ------------------------------------------------------------
    // In this function, you are supposed to get into timeline mode.
    // You may need to call a service method to communicate with
    // the server. Use getPostMessage/displayPostMessage functions 
    // in client.cc file for both getting and displaying messages 
    // in timeline mode.
    // ------------------------------------------------------------

    // ------------------------------------------------------------
    // IMPORTANT NOTICE:
    //
    // Once a user enter to timeline mode , there is no way
    // to command mode. You don't have to worry about this situation,
    // and you can terminate the client program by pressing
    // CTRL-C (SIGINT)
    // ------------------------------------------------------------
  
    /***
    YOUR CODE HERE
    ***/

}



//////////////////////////////////////////////
// Main Function
/////////////////////////////////////////////
int main(int argc, char** argv) {

  std::string hostname = "127.0.0.1";
  std::string username = "default";
  std::string port = "3010";
    
  int opt = 0;
  while ((opt = getopt(argc, argv, "h:u:p:")) != -1){
    switch(opt) {
    case 'h':
      hostname = optarg;break;
    case 'u':
      username = optarg;break;
    case 'p':
      port = optarg;break;
    default:
      std::cout << "Invalid Command Line Argument\n";
    }
  }
      
  std::cout << "Logging Initialized. Client starting..."<<std::endl;
  
  Client myc(hostname, username, port);
  
  myc.run();
  
  return 0;
}