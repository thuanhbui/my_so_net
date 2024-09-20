/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <fstream>
#include <iostream>
#include <filesystem>
#include <memory>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include "sns.grpc.pb.h"


using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce662::Message;
using csce662::ListReply;
using csce662::Request;
using csce662::Reply;
using csce662::SNSService;


struct Client {
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client*> client_followers;
  std::vector<Client*> client_following;
  ServerReaderWriter<Message, Message>* stream = 0;
  bool operator==(const Client& c1) const{
    return (username == c1.username);
  }
};

//Vector that stores every client that has been created
std::vector<Client*> client_db;

//search if an user is already registered in client_db
int lookup_user(std::string username) {
	for (int i = 0; i < client_db.size(); i++) {
		if (client_db[i]->username == username)
			return i;
	}
	return -1;
}

class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {

    std::string username = request->username();
    LOG(INFO) <<"User: " << username << " -> RPC: List";

    Client* user = client_db[lookup_user(username)];
    LOG(INFO)<<"Followers: " <<user->client_followers.size();
    for (Client* c : user->client_followers) {
	    list_reply->add_followers(c->username);
    }

    LOG(INFO)<<"Users: " <<client_db.size();
    for (Client* c : client_db) {
	    list_reply->add_all_users(c->username);
    }

    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {

    std::string current_username = request->username();
    std::string follow_username = request->arguments(0);
    LOG(INFO) <<"User: "<< current_username <<" -> RPC: Follow | argument: " <<follow_username;

    if (current_username == follow_username) {
	    LOG(INFO) <<"Invalid username: User cannot follow themselves.";
	    reply->set_msg("Invalid username: You cannot follow yourself!");
	    return Status::OK;
    }

    int follow_user_index = lookup_user(follow_username);
    if (follow_user_index < 0) {
	    LOG(INFO) <<"Invalid username: user not found.";
	    reply->set_msg("Invalid username: User not found.");
	    return Status::OK;
    }

    Client* current_user = client_db[lookup_user(current_username)];
    Client* follow_user = client_db[follow_user_index];

    auto find = std::find(current_user->client_following.begin(), current_user->client_following.end(), follow_user);
    if (find != current_user->client_following.end()) {
	    LOG(INFO) <<"User already followed";
	    reply->set_msg("You already followed this user!");
	    return Status::OK;
    } else {
	    current_user->client_following.push_back(follow_user);
	    follow_user->client_followers.push_back(current_user);
	    LOG(INFO) <<"Follow successfully";
	    reply->set_msg("Follow successfully!");
	    return Status::OK;
    }
    	  
    return Status::OK; 
  }
  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {

    std::string username1 = request->username();
    std::string username2 = request->arguments(0);
    LOG(INFO) <<"User: " << username1 << " -> RPC: UnFollow | argument: " << username2;

    if (username1 == username2) {
	    LOG(INFO) <<"Invalid username: User cannot unfollow themselves.";
	    reply->set_msg("Invalid username: You cannot unfollow yourself!");
	    return Status::OK;
    }

    int user2_index = lookup_user(username2);
    if (user2_index < 0) {
	    LOG(INFO) << "Invalid username: User not found.";
	    reply->set_msg("Invalid username: User not found.");
	    return Status::OK;
    }

    Client* user1 = client_db[lookup_user(username1)];
    Client* user2 = client_db[user2_index];

    auto find = std::find(user1->client_following.begin(), user1->client_following.end(), user2);
    if (find != user1->client_following.end()) {
	    user1->client_following.erase(
			    std::remove(user1->client_following.begin(), user1->client_following.end(), user2), 
			    user1->client_following.end());
	    user2->client_followers.erase(
			    std::remove(user2->client_followers.begin(), user2->client_followers.end(), user1),
			    user2->client_followers.end());
	    LOG(INFO) << "UnFollow successfully!";
	    reply->set_msg("UnFollow sucessfully!");
	    return Status::OK;
    } else {
	    LOG(INFO) << "User not a follower.";
	    reply->set_msg("You are not a follower");
	    return Status::OK;
    }
    return Status::OK;
  }

  // RPC Login
  Status Login(ServerContext* context, const Request* request, Reply* reply) override {

    std::string username = request->username();
    LOG(INFO) <<"User: " <<username <<" -> RPC: Login";  

    int user_index = lookup_user(username);
    if (user_index >= 0) {
	    Client* user = client_db[user_index];
	    if (user->connected) {
		    LOG(INFO)<<"User is already logged in";
		    reply->set_msg("You are already logged in.");
	    } else {
		    LOG(INFO)<< "User login successfully";
		    user->connected = true;
		    reply->set_msg("Login successfully!");
	    }
    } else {
	    LOG(INFO)<<"Username not found";
	    Client* new_user = new Client();
	    new_user->username = username;
	    client_db.push_back(new_user);
	    LOG(INFO)<<"Added new user! DB size: " << client_db.size();
	    reply->set_msg("Welcome to Tiny SNS, " + username + "!");
    }

    return Status::OK;
  }

  Status Timeline(ServerContext* context, 
		ServerReaderWriter<Message, Message>* stream) override {

    Message msg;
    Client* user;

    //Save post files and timeline files to folder ~/files
    std::string file_directory = "files";
    if (!std::filesystem::exists(file_directory)) {
	    std::filesystem::create_directory(file_directory);
    }

    while (stream->Read(&msg)) {

	    //parse the message
	    std::string username = msg.username();
	    std::string message_content = msg.msg();
	    std::string timestamp = google::protobuf::util::TimeUtil::ToString(msg.timestamp());

	    //lookup user who sends the message
	    LOG(INFO)<<"Username: " << username << " |Message: " << message_content;
	    user = client_db[lookup_user(username)];
	    if (!user->stream) user->stream = stream;

	    std::string posts_filename = file_directory + "/" + username + "_posts.txt";
	    std::ofstream posts_file(posts_filename, std::ios::app | std::ios::out);

	    if (message_content == "Request Timeline") {
		    //Return last 20 messages
		    std::string filename = file_directory + "/" + username + "_timeline.txt";
		    std::ifstream timeline(filename);
		    std::string line;
		    std::vector<Message> messages;
		    
		    int nb_post = std::min(20, user->following_file_size);
		    int nb_ignore = user->following_file_size - nb_post;
		    int post_count = 0;

		    Message msg;
		    std::string msg_t;
		    std::string msg_u;
		    std::string msg_w;

		    if (timeline.is_open()) {
			    //ignore first (total - 20) posts
			    for (int i = 0; i < nb_ignore*4; i++) std::getline(timeline,line);
			    while (std::getline(timeline, line)) {
				    if (line[0] == 'T') msg_t = line.substr(2);
				    else if (line[0] == 'U') msg_u = line.substr(2);
				    else if (line[0] == 'W') msg_w = line.substr(2);
				    else {
 					    msg.set_username(msg_u);
					    msg.set_msg(msg_w);
					    google::protobuf::Timestamp msg_timestamp;
					    google::protobuf::util::TimeUtil::FromString(msg_t, &msg_timestamp);
					    msg.mutable_timestamp()->CopyFrom(msg_timestamp);
					    messages.push_back(msg);
					    
					    msg_t.clear(); 
					    msg_u.clear();
					    msg_w.clear();				    
				    } 
			    }
			    for (int i = messages.size() - 1; i >= 0; i--) {
				    stream->Write(messages[i]);
			    }
		    } else {
			    LOG(ERROR)<<"Failed to open file: " <<filename;
		    }

		    timeline.close();

	    } else {
		    //Append new post to user's local file
		    if (!posts_file.is_open()) {
			    LOG(ERROR) << "Failed to open file: " << posts_filename;
		    } else {
			    std::string file_entry = timestamp + " | " + message_content + "\n";
			    posts_file << file_entry;
			    posts_file.close();
		    }

		    //Process user's followers
		    for (Client* c : user->client_followers) {
			    //Publish new post to online followers
			    if (c->connected && c->stream) {
				    c->stream->Write(msg);
			    }
			    //Append new post to all followers' timeline file
			    std::string timeline_filename = file_directory + "/" + c->username + "_timeline.txt";
			    std::ofstream timeline_file(timeline_filename, std::ios::app | std::ios::out);
			    if (!timeline_file.is_open()) {
				    LOG(ERROR) << "Failed to open file: " << timeline_filename;
			    } else {
				    std::string timeline_entry = "T " + timestamp + "\n"
					    			+ "U " + username + "\n"
								+ "W " + message_content + "\n";
				    timeline_file << timeline_entry;
				    timeline_file.close();
				    c->following_file_size++;
			    }
		    }
	    }

    }
    user->connected = false;
    return Status::OK;
  }

};


void RunServer(std::string port_no) {
  std::string server_address = "127.0.0.1:"+port_no;
  SNSServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  //std::cout << "Server listening on" << server_address << std::endl;
  log(INFO, "Server listening on "+server_address);

  server->Wait();
}

int main(int argc, char** argv) {

  std::string port = "3010";
  
  int opt = 0;
  while ((opt = getopt(argc, argv, "p:")) != -1){
    switch(opt) {
      case 'p':
          port = optarg;break;
      default:
	  std::cerr << "Invalid Command Line Argument\n";
    }
  }
  
  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");
  RunServer(port);

  return 0;
}
