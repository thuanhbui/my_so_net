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
#include <thread>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <mutex>
#include <fcntl.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include "sns.grpc.pb.h"
#include "coordinator.grpc.pb.h"


using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using grpc::ClientContext;
using csce662::Message;
using csce662::ListReply;
using csce662::Request;
using csce662::Reply;
using csce662::SNSService;
using csce662::CoordService;
using csce662::ServerInfo;
using csce662::Confirmation;
using csce662::ID;

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
  std::streampos last_timeline_pos = 0;
};

//Vector that stores every client that has been created
std::vector<Client*> client_db;
std::string base_directory;
ServerInfo serverinfo;
std::unique_ptr<SNSService::Stub> slave_stub_;
std::unique_ptr<CoordService::Stub> stub_;
bool isMaster = false;
//search if an user is already registered in client_db
int lookup_user(std::string username) {
	for (int i = 0; i < client_db.size(); i++) {
		if (client_db[i]->username == username)
			return i;
	}
	return -1;
}

int lookup_follower(std::string username, std::vector<Client*> followers) {
	for (int i = 0; i < followers.size(); i++) {
		if (followers[i]->username == username)
			return i;
	}
	return -1;
}

void getSlave();
std::vector<std::string> get_lines_from_file(std::string);
std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {

    std::string username = request->username();
    log(INFO, "User: " + username + " -> RPC: List");

    Client* user = client_db[lookup_user(username)];
    log(INFO, "Followers: " + std::to_string(user->client_followers.size()));
    for (Client* c : user->client_followers) {
	    list_reply->add_followers(c->username);
    }

    log(INFO, "Users: " + std::to_string(client_db.size()));
    for (Client* c : client_db) {
	    list_reply->add_all_users(c->username);
    }

    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {
    
    if (isMaster) {
	    getSlave();
	    if (slave_stub_ != nullptr) {
		ClientContext slaveContext;
	    	Request slaveRequest;
	    	Reply slaveReply;
		slaveRequest.set_username(request->username());
		slaveRequest.add_arguments(request->arguments(0));
	    	Status status = slave_stub_->Follow(&slaveContext, slaveRequest, &slaveReply);
	    }
    }

    std::string current_username = request->username();
    std::string follow_username = request->arguments(0);
    log(INFO, "User: " + current_username + " -> RPC: Follow | argument: " + follow_username);

    if (current_username == follow_username) {
	    log(INFO, "Invalid username: User cannot follow themselves.");
	    reply->set_msg("Invalid username: You cannot follow yourself!");
	    return Status::OK;
    }

    int follow_user_index = lookup_user(follow_username);
    if (follow_user_index < 0) {
	    log(INFO,"Invalid username: user not found.");
	    reply->set_msg("Invalid username: User not found.");
	    return Status::OK;
    }

    Client* current_user = client_db[lookup_user(current_username)];
    Client* follow_user = client_db[follow_user_index];

    auto find = std::find(current_user->client_following.begin(), current_user->client_following.end(), follow_user);
    if (find != current_user->client_following.end()) {
	    log(INFO,"User already followed");
	    reply->set_msg("You already followed this user!");
	    return Status::OK;
    } else {
	    current_user->client_following.push_back(follow_user);
	    follow_user->client_followers.push_back(current_user);

	    std::string following_filename = base_directory + "/" + serverinfo.type() + "/" + current_username + "_following.txt";
	    std::ofstream following_file(following_filename, std::ios::app | std::ios::out);
	    if (following_file.is_open()) {
		    std::string following_entry = follow_username + "\n";
		    following_file << following_entry;
		    following_file.close();
	    } else {
		    log(ERROR, "Failed to open Following file");
	    }

	    bool same_cluster = (std::atoi(current_username.c_str()) % 3) == (std::stoi(follow_username.c_str()) % 3);
	    if (same_cluster) {
                    std::string followers_filename = base_directory + "/" + serverinfo.type() + "/" + follow_username + "_followers.txt";
		    std::ofstream followers_file(followers_filename, std::ios::app | std::ios::out);
		    if (followers_file.is_open()) {
			    std::string followers_entry = current_username + "\n";
			    followers_file << followers_entry;
			    followers_file.close();
		    } else {
			    log(ERROR, "Failed to open Followers file");
		    }
	    }

	    log(INFO, "Follow successfully");
	    reply->set_msg("Follow successfully!");
	    return Status::OK;
    }
    	  
    return Status::OK; 
  }
  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {

    std::string username1 = request->username();
    std::string username2 = request->arguments(0);
    log(INFO, "User: " + username1 + " -> RPC: UnFollow | argument: " + username2);

    if (username1 == username2) {
	    log(INFO, "Invalid username: User cannot unfollow themselves.");
	    reply->set_msg("Invalid username: You cannot unfollow yourself!");
	    return Status::OK;
    }

    int user2_index = lookup_user(username2);
    if (user2_index < 0) {
	    log(INFO,"Invalid username: User not found.");
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
	    log(INFO,"UnFollow successfully!");
	    reply->set_msg("UnFollow sucessfully!");
	    return Status::OK;
    } else {
	    log(INFO, "User not a follower.");
	    reply->set_msg("You are not a follower");
	    return Status::OK;
    }
    return Status::OK;
  }

  // RPC Login
  Status Login(ServerContext* context, const Request* request, Reply* reply) override {

    if (isMaster) {
	    getSlave();
	    if (slave_stub_ != nullptr) {
		ClientContext slaveContext;
	    	Request slaveRequest;
	    	Reply slaveReply;
		slaveRequest.set_username(request->username());
	    	Status status = slave_stub_->Login(&slaveContext, slaveRequest, &slaveReply);
	    }
    }
  
    std::string username = request->username();
    log(INFO, "User: "  + username + " -> RPC: Login");
    
    int user_index = lookup_user(username);
    if (user_index >= 0) {
	    Client* user = client_db[user_index];
	    if (user->connected) {
		    log(INFO, "User is already logged in");
		    reply->set_msg("You are already logged in.");
	    } else {
		    log(INFO, "User login successfully");
		    if (isMaster) user->connected = true;
		    reply->set_msg("Login successfully!");
	    }
    } else {
	    log(INFO, "Username not found");

	    Client* new_user = new Client();
	    new_user->username = username;
	    if (!isMaster) new_user->connected = false;
	    client_db.push_back(new_user);

	    std::string file_directory = base_directory + "/" + serverinfo.type();
	    if (!std::filesystem::exists(file_directory)) {
		    std::filesystem::create_directories(file_directory);
	    }
	    std::string all_users_filename = file_directory + "/" + "all_users.txt";
	    std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + all_users_filename;
	    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
	    std::ofstream all_users_file(all_users_filename, std::ios::app | std::ios::out);
	    if (all_users_file.is_open()) {
		    std::string user_entry = username + "\n";
		    all_users_file << user_entry;
		    all_users_file.close();
		    sem_close(fileSem);
	    }

	    log(INFO, "Added new user! DB size: " + std::to_string(client_db.size()));

	    reply->set_msg("Welcome to Tiny SNS, " + username + "!");
    }

    return Status::OK;
  }

  Status SlaveAddPost(ServerContext* context, const Request* request, Reply* reply) {
  	std::string file_directory = base_directory + "/" + serverinfo.type();
        if (!std::filesystem::exists(file_directory)) {
            std::filesystem::create_directories(file_directory);
        }
	std::string username = request->username();
	std::string timeline_entry = request->arguments(0);
	std::string user_timeline_filename = file_directory + "/" + username + "_timeline.txt";
        std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + user_timeline_filename;
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
        std::ofstream user_timeline_file(user_timeline_filename, std::ios::app | std::ios::out);
        //Append new post to slave's local file
        if (!user_timeline_file.is_open()) {
             log(ERROR, "Failed to open file: " + user_timeline_filename);
             reply->set_msg("Failed to add post at Slave");		    
        } else {
             user_timeline_file << timeline_entry;
             user_timeline_file.close();
             sem_close(fileSem);
        }
	reply->set_msg("Add post at Slave successfully!");
	return Status::OK;

  }

  Status Timeline(ServerContext* context, 
		ServerReaderWriter<Message, Message>* stream) override {

    Message msg;
    Client* user;

    //Save post files and timeline files to folder ~/files
    std::string file_directory = base_directory + "/" + serverinfo.type();
    if (!std::filesystem::exists(file_directory)) {
	    std::filesystem::create_directories(file_directory);
    }

    while (stream->Read(&msg)) {

	    //parse the message
	    std::string username = msg.username();
	    std::string message_content = msg.msg();
	    std::string timestamp = google::protobuf::util::TimeUtil::ToString(msg.timestamp());
	    timestamp[timestamp.find('T')] = ' '; // remove T
	    timestamp.pop_back(); // remove Z

	    //lookup user who sends the message
	    log(INFO, "Username: " + username + " |Message: " + message_content);
	    user = client_db[lookup_user(username)];
	    if (!user->stream) user->stream = stream;

	    if (message_content == "Request Timeline") {
		    //Return last 20 messages
		    std::string filename = file_directory + "/" + username + "_timeline.txt";
		    std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + filename;
		    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
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
				    else if (line[0] == 'U') msg_u = line.substr(21);
				    else if (line[0] == 'W') msg_w = line.substr(2);
				    else {
 					    msg.set_username(msg_u);
					    msg.set_msg(msg_w);
					    std::replace(msg_t.begin(), msg_t.end(), ' ', 'T');
    					    msg_t += "Z";
					    google::protobuf::Timestamp msg_timestamp;
					    google::protobuf::util::TimeUtil::FromString(msg_t, &msg_timestamp);
					    msg.mutable_timestamp()->CopyFrom(msg_timestamp);
					    messages.push_back(msg);

					    log(INFO, "time: " + msg_t);
					    
					    msg_t.clear(); 
					    msg_u.clear();
					    msg_w.clear();				    
				    } 
			    }
			    for (int i = 0; i < messages.size(); i++) {
				    stream->Write(messages[i]);
			    }
		    } else {
			    log(ERROR, "Failed to open file: " + filename);
		    }

		    timeline.close();
		    sem_close(fileSem);
	    } else {
		    std::string user_timeline_filename = file_directory + "/" + username + "_timeline.txt";
		    std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + user_timeline_filename;
		    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
	    	    std::ofstream user_timeline_file(user_timeline_filename, std::ios::app | std::ios::out);

                    std::string timeline_entry = "T " + timestamp + "\n"
			    			+ "U http://twitter.com/" + username + "\n"
						+ "W " + message_content + "\n";
 
		    //Append new post to user's local file
		    if (!user_timeline_file.is_open()) {
			    log(ERROR, "Failed to open file: " + user_timeline_filename);
		    } else {
			    user_timeline_file << timeline_entry;
			    user->last_timeline_pos = user_timeline_file.tellp();
			    user_timeline_file.close();
			    sem_close(fileSem);
		    }

		    //Inform Slave
		    if (isMaster) {
		    	getSlave();
			if (slave_stub_ != nullptr) {
				ClientContext slaveContext;
				Request slaveRequest;
				Reply slaveReply;
				slaveRequest.set_username(user->username);
				slaveRequest.add_arguments(timeline_entry);
				Status status = slave_stub_->SlaveAddPost(&slaveContext, slaveRequest, &slaveReply);
			}
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
				    log(ERROR, "Failed to open file: " + timeline_filename);
			    } else {
				    timeline_file << timeline_entry;
				    timeline_file.close();
				    c->following_file_size++;
			    }
		    }
	    }

    }
    user->connected = false;
    user->stream = 0;
    return Status::OK;
  }

};


class ServerProvider {
  public: 
     ServerProvider(const std::string c_id, const std::string s_id, const std::string hname, const std::string p, 
		     const std::string cdnt_ip, const std::string cdnt_port):
	     cluster_id(c_id), server_id(s_id), hostname(hname), port(p), coord_ip(cdnt_ip), coord_port(cdnt_port) {}
     ~ServerProvider() {
	     hb_thread.join();
	     sync_users_thread.join();
	     sync_relations_thread.join();
     }
  void run() {
	  setUpWithCoordinator();
	  RunServer();
  }
  private:
     std::string cluster_id;
     std::string server_id;
     std::string hostname;
     std::string port;
     std::string coord_ip;
     std::string coord_port;

     std::thread hb_thread;
     std::thread sync_users_thread;
     std::thread sync_relations_thread;
     std::thread sync_timeline_thread;

     int setUpWithCoordinator();
     void RunServer();
     void sendHeartBeat();
     void updateUserList();
     void updateClientsRelations();
     void updateTimeline();
     void update_timeline_of_user(std::string filename, Client* c);
     void update_followers_of_user(std::string file_path, std::vector<Client*>& list);
};

int ServerProvider::setUpWithCoordinator() {

     std::string coord_address = coord_ip + ":" + coord_port;
     auto channel = grpc::CreateChannel(coord_address, grpc::InsecureChannelCredentials());
     stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(channel));
     
     serverinfo.set_serverid(std::stoi(server_id));
     serverinfo.set_hostname(hostname);
     serverinfo.set_port(port);
     serverinfo.set_type("new");
     serverinfo.set_clusterid(std::stoi(cluster_id));
     
     hb_thread = std::thread(&ServerProvider::sendHeartBeat, this);
     sync_users_thread = std::thread(&ServerProvider::updateUserList, this);
     sync_relations_thread = std::thread(&ServerProvider::updateClientsRelations, this);
     sync_timeline_thread = std::thread(&ServerProvider::updateTimeline, this);

     //Set up files folder
     base_directory = "files/cluster_" + std::to_string(serverinfo.clusterid());
     if (!std::filesystem::exists(base_directory)) {
	    std::filesystem::create_directories(base_directory);
     }

     return 0;
}

void ServerProvider::sendHeartBeat() {
     while (true) {
     	ClientContext context;
     	Confirmation confirmation;
     	Status status = stub_->Heartbeat(&context, serverinfo, &confirmation);
     	if (!status.ok() || !confirmation.status()) {
		log(ERROR, "Cannot get confirmaton from Coordinator!");
		exit(-1);
     	}
     	log(INFO, "Got confirmation from Coordinator");
	if (serverinfo.type() == "new") {
		serverinfo.set_type(confirmation.type());
		if (confirmation.type() == "1") isMaster = true;
		else isMaster = false;
	} else if (serverinfo.type() == "2" && confirmation.type() == "1") {
		//TODO transfer Master rights to Slave
		log(INFO, "Change type to Master");
		//serverinfo.set_type(confirmation.type());
		isMaster = true;
	}
	std::this_thread::sleep_for(std::chrono::seconds(10));
     }	     
}

void ServerProvider::updateUserList() {
	bool first_fetch = true;
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		struct stat fileStat;
		std::string file_path = base_directory + "/" + serverinfo.type() + "/all_users.txt";
		const char* file_name = file_path.c_str();
		if (stat(file_name, &fileStat) == 0) {
			time_t m_time = fileStat.st_mtime;
			if (difftime(getTimeNow(), m_time) < 10 || first_fetch) {
				log(INFO, "User list changed!");
				std::vector<std::string> user_list = get_lines_from_file(file_path);
				for (std::string user : user_list) {
					//log(INFO, "User list from file: " + user);
					if (lookup_user(user) == -1) {
					   Client* new_user = new Client();
            			           new_user->username = user;
					   new_user->connected = false;
					   client_db.push_back(new_user);
					}
				}
				if (first_fetch) {
					for (Client* user : client_db) {
					      std::string followers_path = base_directory + "/" + serverinfo.type() + "/" + user->username + "_followers.txt";
					      std::string following_path = base_directory + "/" + serverinfo.type() + "/" + user->username + "_following.txt";
					      update_followers_of_user(followers_path, user->client_followers);
					      update_followers_of_user(following_path, user->client_following);
					}

				}
				first_fetch = false;
			}
		} else {
			log(ERROR, "File not available now. File path: " + file_path);
		}
	}
}

void ServerProvider::updateClientsRelations() {
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		std::string path = base_directory + "/" + serverinfo.type() + "/";

		for (Client* c : client_db) {
            		int c_cluster = (std::stoi(c->username) - 1)%3 + 1;
			if (c_cluster != serverinfo.clusterid()) continue;
			std::string file_path = path + c->username + "_followers.txt";
			const char* file_name = file_path.c_str();
			struct stat fileStat;
			if (stat(file_name, &fileStat) == 0) {
				time_t m_time = fileStat.st_mtime;
				if (difftime(getTimeNow(), m_time) < 10) {
					log(INFO, "Followers list changed for user " + c->username);
					update_followers_of_user(file_path, c->client_followers);
				}
				
    			} else {
				log(ERROR, "File not available now. File path: " + file_path);
			}

		}	
	}
}

void ServerProvider::updateTimeline() {
	while (true) {
		std::this_thread::sleep_for(std::chrono::seconds(5));
		std::string path = base_directory + "/" + serverinfo.type() + "/";
		for (Client* c : client_db) {
			int c_cluster = (std::stoi(c->username) - 1)%3 + 1;
			if (c_cluster != serverinfo.clusterid()) continue;
			if (!c->stream) continue;
			std::string file_path = path + c->username + "_timeline.txt";
			const char* file_name = file_path.c_str();
			struct stat fileStat;
			if (stat(file_name, &fileStat) == 0) {
				time_t m_time = fileStat.st_mtime;
			    time_t t_now = getTimeNow();
                            std::string nowtime = ctime(&t_now);
			    std::string protime = ctime(&m_time);
				if (difftime(getTimeNow(), m_time) < 7) {
					log(INFO, "Timeline updated for user " + c->username);
					update_timeline_of_user(file_path, c);	
				}
			}
		}
	}
}

void ServerProvider::update_timeline_of_user(std::string filename, Client* c) {
	std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + filename;
	sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
	std::ifstream timeline(filename);
	std::string line;
	Message msg;
	std::string msg_t;
	std::string msg_u;
	std::string msg_w;
	int sed_cluster = (std::stoi(c->username) - 1)%3 + 1;
	if (timeline.is_open()) {
		timeline.seekg(c->last_timeline_pos);
		while (std::getline(timeline, line)) {
			if (line[0] == 'T') msg_t = line.substr(2);
			else if (line[0] == 'U') msg_u = line.substr(21);
			else if (line[0] == 'W') msg_w = line.substr(2);
			else {
				msg.set_username(msg_u);
				msg.set_msg(msg_w);
				std::replace(msg_t.begin(), msg_t.end(), ' ', 'T');
				msg_t += "Z";
				google::protobuf::Timestamp msg_timestamp;
				google::protobuf::util::TimeUtil::FromString(msg_t, &msg_timestamp);
				msg.mutable_timestamp()->CopyFrom(msg_timestamp);
				int rev_cluster = ((std::stoi(msg_u)-1)%3) + 1;
				if (c->connected && c->stream && rev_cluster != sed_cluster) c->stream->Write(msg);
				msg_t.clear(); 
				msg_u.clear();
				msg_w.clear();
				c->last_timeline_pos = timeline.tellg();
			}
		}
	}
	timeline.close();
	sem_close(fileSem);
}

void ServerProvider::update_followers_of_user(std::string file_path, std::vector<Client*>& list) {
	std::vector<std::string> user_list = get_lines_from_file(file_path);
	for (std::string username : user_list) {
		if (lookup_follower(username, list) == -1) {
			int new_user_index = lookup_user(username);
			if (new_user_index >= 0) {
				Client* new_user = client_db[new_user_index];
				list.push_back(new_user);
			}
		}
	}
}

void ServerProvider::RunServer() {
	std::string server_address = hostname + ":" + port;
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

  std::string coord_ip = "127.0.0.1";
  std::string coord_port = "3010";
  std::string cluster_id = "1";
  std::string server_id = "1";
  std::string port = "10000";
  std::string hostname = "localhost";
  
  int opt = 0;
  while ((opt = getopt(argc, argv, "c:s:h:k:p:")) != -1) {
    switch(opt) {
      case 'c':
  	  cluster_id = optarg;
	  break;
      case 's':
	  server_id = optarg;
	  break;
      case 'h':
          coord_ip = optarg;
	  break;
      case 'k':
	  coord_port = optarg;
	  break;	  
      case 'p':
          port = optarg;
	  break;
      default:
	  std::cerr << "Invalid Command Line Argument\n";
    }
  }

  ServerProvider server(cluster_id, server_id, hostname, port, coord_ip, coord_port);
  server.run();
  
  std::string log_file_name = std::string("server-") + port;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");

  return 0;
}

std::vector<std::string> get_lines_from_file(std::string filename)
{
    log(INFO, "test error");
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    std::string semName = "/" + base_directory + "/" + serverinfo.type() + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    file.open(filename);

    if (file.peek() == std::ifstream::traits_type::eof())
    {
        // return empty vector if empty file
        // std::cout<<"returned empty vector bc empty file"<<std::endl;
        file.close();
        sem_close(fileSem);
        return users;
    }
    while (file)
    {
        getline(file, user);

        if (!user.empty())
            users.push_back(user);
    }

    file.close();
    sem_close(fileSem);

    return users;
}

void getSlave() {
        ClientContext context;
        ServerInfo slave;
        ID id;
        id.set_id(serverinfo.clusterid());
        Status status = stub_->GetSlave(&context, id, &slave);
        if (!status.ok()) {
             log(INFO, "GRPC failed when getting Slave");
             slave_stub_ = nullptr;
        } else {
                log(INFO, "Successfully get Slave. Configuring Slave Stub ...");
                std::string hostname = slave.hostname();
                std::string port = slave.port();
                std::string slave_address = hostname + ":" + port;

                auto slave_channel = grpc::CreateChannel(slave_address, grpc::InsecureChannelCredentials());
                slave_stub_ = std::unique_ptr<SNSService::Stub>(SNSService::NewStub(slave_channel));
                log(INFO, "Finish configuring Slave Stub");
        }
}

