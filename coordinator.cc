#include <algorithm>
#include <cstdio>
#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity);

#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce662::CoordService;
using csce662::ServerInfo;
using csce662::Confirmation;
using csce662::ID;
using csce662::ServerList;
using csce662::SynchService;

struct zNode{
    int serverID;
    std::string hostname;
    std::string port;
    std::string type;
    std::time_t last_heartbeat;
    bool missed_heartbeat;
    bool isActive();

};

//potentially thread safe 
std::mutex v_mutex;
std::vector<zNode*> cluster1;
std::vector<zNode*> cluster2;
std::vector<zNode*> cluster3;
std::vector<bool> master_die = {false, false, false};

// creating a vector of vectors containing znodes
std::vector<std::vector<zNode*>> clusters = {cluster1, cluster2, cluster3};

//func declarations
int findServer(std::vector<zNode*> v, int id); 
std::time_t getTimeNow();
void checkHeartbeat();


bool zNode::isActive(){
    bool status = false;
    if(!missed_heartbeat){
        status = true;
    }else if(difftime(getTimeNow(),last_heartbeat) < 10){
        status = true;
    }
    return status;
}

int lookup_server(int serverID, std::vector<zNode*> cluster) {
    for (int i = 0; i < cluster.size(); i++) {
	    if (cluster[i]->serverID == serverID) 
		    return i;
    }
    return -1;
}

int lookup_synchronizer(int synchID, std::vector<zNode*> cluster) {
	for (int i = 0; i < cluster.size(); i++) {
		if (cluster[i]->serverID == synchID && cluster[i]->type == "synchronizer") {
			return i;
		}
	}
	return -1;
}

class CoordServiceImpl final : public CoordService::Service {

    Status Heartbeat(ServerContext* context, const ServerInfo* serverinfo, Confirmation* confirmation) override {
        // Your code here
	int server_id = serverinfo->serverid();
	LOG(INFO) << "Cluster ID: " <<serverinfo->clusterid() << "; Server ID: " << server_id << "-> RPC: Heartbeat";
	int cluster_index = serverinfo->clusterid() - 1;
	if (cluster_index > 2) {
		LOG(INFO) <<"Cluster ID not found";
		return grpc::Status(grpc::StatusCode::NOT_FOUND, "Cluster ID not found.");
	}
	int server_index = lookup_server(server_id, clusters[cluster_index]);
	if (serverinfo->type() == "synchronizer") {
		server_index = lookup_synchronizer(server_id, clusters[cluster_index]);
		if (server_index >= 0) {
			zNode* synchronizer = clusters[cluster_index][server_index];
			synchronizer->last_heartbeat = getTimeNow();
		} else {
			log(INFO, "Adding new synchronizer to Cluster " + std::to_string(serverinfo->clusterid()));
			zNode* synchronizer = new zNode();
			synchronizer->serverID = server_id;
			synchronizer->hostname = serverinfo->hostname();
			synchronizer->port = serverinfo->port();
			synchronizer->type = "synchronizer";
			synchronizer->last_heartbeat = getTimeNow();
			clusters[cluster_index].push_back(synchronizer);	
		}
		if ((server_id == serverinfo->clusterid() && !master_die[cluster_index]) 
				|| (server_id != serverinfo->clusterid() && master_die[cluster_index])) {
			confirmation->set_type("1");
		} else {
			confirmation->set_type("2");
		}
		confirmation->set_status(true);
		return Status::OK;
	}

	if (server_index >= 0) {
		zNode* server = clusters[cluster_index][server_index];
		server->last_heartbeat = getTimeNow();
		/*
		if (clusters[cluster_index][0]->type == "dead") {
			log(INFO, "Switch Slave to Master");
			confirmation->set_type("1");
			server->type = "1";
		} else confirmation->set_type(serverinfo->type());
		*/
		if (server->type == "2" && server_index == 0) {
			log(INFO, "Change Slave type to Master");
			confirmation->set_type("1");
			server->type = "1";
			master_die[cluster_index] = true;
		}
		else confirmation->set_type(serverinfo->type());
	} else {
		LOG(INFO) <<"Adding new server to Cluster " << serverinfo->clusterid() 
			<< ": "<<serverinfo->hostname()
			<< ":"<<serverinfo->port()
			<<"; "<<serverinfo->type();
		zNode* server = new zNode();
		server->serverID = server_id;
		server->hostname = serverinfo->hostname();
		server->port = serverinfo->port();
		if (clusters[cluster_index].size() == 0) {
			server->type = "1";
			confirmation->set_type("1");
		} else {
			server->type = "2";
			confirmation->set_type("2");
		}
		server->last_heartbeat = getTimeNow();
		clusters[cluster_index].push_back(server);
	}

	confirmation->set_status(true);

        return Status::OK;
    }

    //function returns the server information for requested client id
    //this function assumes there are always 3 clusters and has math
    //hardcoded to represent this.
    Status GetServer(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        // Your code here
	int client_id = id->id();
	log(INFO, "Get Server for Client ID: " + std::to_string(client_id));
	int cluster_id = ((client_id - 1) % 3) + 1;
	LOG(INFO) << "Cluster ID: " << cluster_id;
	if (cluster_id > 3) return grpc::Status(grpc::StatusCode::NOT_FOUND, "Cluster ID not found");

	for (zNode* s : clusters[cluster_id - 1]) {
	    if (s->isActive() && s->type == "1") {
		serverinfo->set_serverid(s->serverID);
		serverinfo->set_hostname(s->hostname);
		serverinfo->set_port(s->port);
		serverinfo->set_type(s->type);
		serverinfo->set_clusterid(cluster_id);
		return Status::OK;
	    }
	}
	
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Server is not available now");
    }

    Status GetSlave(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
	    log(INFO, "Get Slave for Cluster ID: " + std::to_string(id->id()));
	    int cluster_id = id->id();
	    if (cluster_id > 3) return grpc::Status(grpc::StatusCode::NOT_FOUND, "Cluster ID not found");

	    for (zNode* s : clusters[cluster_id - 1]) {
		    if (s->isActive() && s->type == "2") {
			    serverinfo->set_serverid(s->serverID);
			    serverinfo->set_hostname(s->hostname);
			    serverinfo->set_port(s->port);
			    serverinfo->set_type(s->type);
			    serverinfo->set_clusterid(cluster_id);
			    return Status::OK;
		    }
	    }
	    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No Slave found in the cluster now");

    }

    Status GetAllFollowerServers(ServerContext* context, const ID* id, ServerList* serverlist) override {
    	log(INFO, "Get Follower Synchronizer list for Synchronizer ID: " + std::to_string(id->id()));
	int cluster_index = ((id->id() - 1) % 3);
	for (int i = 0; i < clusters.size(); i++) {
		for (auto& s : clusters[i]) {
			if (s->type == "synchronizer" && cluster_index != i) {
				serverlist->add_serverid(s->serverID);
				serverlist->add_hostname(s->hostname);
				serverlist->add_port(s->port);
				serverlist->add_type(s->type);
			}
		}
	}
	return Status::OK;
    }


};

void RunServer(std::string port_no){
    //start thread to check heartbeats
    std::thread hb(checkHeartbeat);
    //localhost = 127.0.0.1
    std::string server_address("127.0.0.1:"+port_no);
    CoordServiceImpl service;
    //grpc::EnableDefaultHealthCheckService(true);
    //grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {

    std::string port = "3010";
    int opt = 0;
    while ((opt = getopt(argc, argv, "p:")) != -1){
        switch(opt) {
            case 'p':
                port = optarg;
                break;
            default:
                std::cerr << "Invalid Command Line Argument\n";
        }
    }
    RunServer(port);
    return 0;
}



void checkHeartbeat(){
    while(true){
        //check servers for heartbeat > 10
        //if true turn missed heartbeat = true
        // Your code below

        v_mutex.lock();

        // iterating through the clusters vector of vectors of znodes
        for (auto& c : clusters){
            for(auto& s : c){
                if(difftime(getTimeNow(),s->last_heartbeat)>10){
                    std::cout << "missed heartbeat from server " << s->serverID << std::endl;
                    if(!s->missed_heartbeat){
                        s->missed_heartbeat = true;
                        //s->last_heartbeat = getTimeNow();
                    } else {
			//missed last hearbeat, and miss this hearbeat: master die
			if (s->type == "1") {
				//log(INFO, "Master DEAD: " + s->hostname + ":" + s->port);
				//s->type == "dead";
				c.erase(find(c.begin(), c.end(), s));
			}
		    }
                } else {
			s->missed_heartbeat = false;
		}
            }
        }

        v_mutex.unlock();

        sleep(10);
    }
}


std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

