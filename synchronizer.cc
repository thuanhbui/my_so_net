// NOTE: This starter code contains a primitive implementation using the default RabbitMQ protocol.
// You are recommended to look into how to make the communication more efficient,
// for example, modifying the type of exchange that publishes to one or more queues, or
// throttling how often a process consumes messages from a queue so other consumers are not starved for messages
// All the functions in this implementation are just suggestions and you can make reasonable changes as long as
// you continue to use the communication methods that the assignment requires between different processes

#include <bits/fs_fwd.h>
#include <ctime>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <stdio.h>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <tuple>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#include "sns.grpc.pb.h"
#include "sns.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <jsoncpp/json/json.h>

#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

namespace fs = std::filesystem;

using csce662::AllUsers;
using csce662::Confirmation;
using csce662::CoordService;
using csce662::ID;
using csce662::ServerInfo;
using csce662::ServerList;
using csce662::SynchronizerListReply;
using csce662::SynchService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
// tl = timeline, fl = follow list
using csce662::TLFL;

int synchID = 1;
int clusterID = 1;
bool isMaster = false;
int total_number_of_registered_synchronizers = 0; // update this by asking coordinator
std::string coordAddr;
std::string clusterSubdirectory;
std::vector<std::string> otherHosts;
std::unordered_map<std::string, int> timelineLengths;

std::vector<std::string> get_lines_from_file(std::string);
std::vector<std::string> get_all_users_func(int);
std::vector<std::string> get_all_users_func_pub(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getFollowersOfUser(int);
std::vector<std::string> getFollowersOfUser_pub(int);
std::vector<std::string> getAllFollowers(int);
bool file_contains_user(std::string filename, std::string user);
bool file_contains_post(std::string filename, std::string t, std::string u, std::string w);
std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}
std::time_t getProtoTime(std::string t) {
     std::string msg_t = t.substr(2);
     std::replace(msg_t.begin(), msg_t.end(), ' ', 'T');
     msg_t += "Z";
     google::protobuf::Timestamp msg_timestamp;
     google::protobuf::util::TimeUtil::FromString(msg_t, &msg_timestamp);
     std::time_t time_in_seconds = msg_timestamp.seconds();
     return time_in_seconds;
}

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID);

std::unique_ptr<csce662::CoordService::Stub> coordinator_stub_;

class SynchronizerRabbitMQ
{
private:
    amqp_connection_state_t conn;
    amqp_channel_t users_channel;
    amqp_channel_t relations_channel;
    amqp_channel_t timeline_channel;
    std::string hostname;
    int port;
    int synchID;

    void setupRabbitMQ()
    {
        conn = amqp_new_connection();
        amqp_socket_t *socket = amqp_tcp_socket_new(conn);
        amqp_socket_open(socket, hostname.c_str(), port);
        amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
	amqp_channel_open(conn, users_channel);
	//amqp_channel_open(conn, relations_channel);
	//amqp_channel_open(conn, timeline_channel);
    }

    void declareQueue(const std::string &queueName, amqp_channel_t channel)
    {
        amqp_queue_declare(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, 1, 0, amqp_empty_table);
    }

    void declareExchange(const std::string &exchangeName, amqp_channel_t channel) {
    	amqp_exchange_declare(conn, channel, amqp_cstring_bytes(exchangeName.c_str()),
			amqp_cstring_bytes("fanout"), 0, 0, 1, 0 ,amqp_empty_table);
    }

    void bindExchange(const std::string &exName, const std::string &queueName, amqp_channel_t channel) {
    	amqp_queue_bind(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                amqp_cstring_bytes(exName.c_str()), amqp_empty_bytes, amqp_empty_table);
    }


    void publishMessage(const std::string &exchangeName, const std::string &message, amqp_channel_t channel)
    {
	std::string wrapper_msg = std::to_string(clusterID) +  "#" + message;
	int res = amqp_basic_publish(conn, channel, amqp_cstring_bytes(exchangeName.c_str()), amqp_empty_bytes, 
			0, 0, NULL, amqp_cstring_bytes(wrapper_msg.c_str()));
    }

    void declareConsumer(const std::string &queueName, amqp_channel_t channel) {
    	amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
			amqp_empty_bytes, 0, 1, 0, amqp_empty_table);
    }


    std::tuple<std::string,std::string> consumeMessage(int timeout_ms)
    {
        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(conn);

        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &timeout, 0);
	std::string actual_queue((char *)envelope.exchange.bytes, envelope.exchange.len);	

        if (res.reply_type != AMQP_RESPONSE_NORMAL)
        {
            return std::make_tuple("Error", "");
        }

        std::string message(static_cast<char *>(envelope.message.body.bytes), envelope.message.body.len);
	
	int senderID = std::stoi(message.substr(0, message.find('#')));
	if (senderID == clusterID) return std::make_tuple("Error", "");
	message = message.substr(message.find('#') + 1);

        amqp_destroy_envelope(&envelope);
	log(INFO, "Message from consume function: " + message);
        return std::make_tuple(actual_queue, message);
    }

public:
    SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname(host), port(p), users_channel(1), relations_channel(1), 
	timeline_channel(1), synchID(id)
    {
        setupRabbitMQ();
	declareExchange("ex_users", users_channel);
	declareExchange("ex_relations", relations_channel);
	declareExchange("ex_timeline", timeline_channel);
	declareQueue("synch" + std::to_string(synchID) + "_users_queue", users_channel);
        declareQueue("synch" + std::to_string(synchID) + "_clients_relations_queue", relations_channel);
        declareQueue("synch" + std::to_string(synchID) + "_timeline_queue", timeline_channel);
	bindExchange("ex_users", "synch" + std::to_string(synchID) + "_users_queue", users_channel);
	bindExchange("ex_relations", "synch" + std::to_string(synchID) + "_clients_relations_queue", relations_channel);
	bindExchange("ex_timeline", "synch" + std::to_string(synchID) + "_timeline_queue", timeline_channel);
	declareConsumer("synch" + std::to_string(synchID) + "_users_queue", users_channel);
	declareConsumer("synch" + std::to_string(synchID) + "_clients_relations_queue", relations_channel);
	declareConsumer("synch" + std::to_string(synchID) + "_timeline_queue", timeline_channel);
        // TODO: add or modify what kind of queues exist in your clusters based on your needs
    }

    void publishUserList()
    {
        std::vector<std::string> users = get_all_users_func_pub(synchID);
	if (users.empty()) return;
        std::sort(users.begin(), users.end());
        Json::Value userList;

        for (const auto &user : users)
        {
            userList["users"].append(user);
        }
        Json::FastWriter writer;
        std::string message = writer.write(userList);
	log(INFO, "Publish USERS LIST. Message: " +  message);
        publishMessage("ex_users", message, users_channel);
    }

    void consumeForAll() {
	    std::tuple<std::string, std::string> tup = consumeMessage(1000);
	    std::string ex = std::get<0>(tup);
	    std::string message = std::get<1>(tup);
	    if (ex == "ex_users") consumeUserLists(message);
	    else if (ex == "ex_relations") consumeClientRelations(message);
	    else if (ex == "ex_timeline") consumeTimelines(message);
	    else log(INFO, "No message to consume!");
    }

    void consumeUserLists(std::string message)
    {
        std::vector<std::string> allUsers;
        // YOUR CODE HERE

        // TODO: while the number of synchronizers is harcorded as 6 right now, you need to change this
        // to use the correct number of follower synchronizers that exist overall
        // accomplish this by making a gRPC request to the coordinator asking for the list of all follower synchronizers registered with it
	    log(INFO, "Consume USERS LIST. Message: " + message);
            if (!message.empty())
            {
                Json::Value root;
                Json::Reader reader;
                if (reader.parse(message, root))
                {
                    for (const auto &user : root["users"])
                    {
                        allUsers.push_back(user.asString());
                    }
                }
            }
        updateAllUsersFile(allUsers);
    }

    void publishClientRelations()
    {
        Json::Value relations;
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            int clientId = std::stoi(client);
            std::vector<std::string> followers = getFollowersOfUser_pub(clientId);

            Json::Value followerList(Json::arrayValue);
            for (const auto &follower : followers)
            {
                followerList.append(follower);
            }

            if (!followerList.empty())
            {
                relations[client] = followerList;
            }
        }

        Json::FastWriter writer;
        std::string message = writer.write(relations);
	if (!relations.isNull() && !relations.empty() && message != "null") {
		log(INFO, "Publish USER RELATIONS. Message" + message);	
		publishMessage("ex_relations", message, relations_channel);
	}
    }

    void consumeClientRelations(std::string message)
    {
        std::vector<std::string> allUsers = get_all_users_func(synchID);

        // YOUR CODE HERE

        // TODO: hardcoding 6 here, but you need to get list of all synchronizers from coordinator as before

            //std::string queueName = "synch" + std::to_string(synchID) + "_clients_relations_queue";
            //std::string message = consumeMessage(queueName, 1000, relations_channel); // 1 second timeout
	    log(INFO, "Consume USER RELATIONS. Message: " + message);

            if (!message.empty())
            {
                Json::Value root;
                Json::Reader reader;
                if (reader.parse(message, root))
                {
                    for (const auto &client : allUsers)
                    {
			    std::string folder = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory;
			if (!fs::exists(folder)) { fs::create_directories(folder);}    
                        std::string followerFile = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client + "_followers.txt";
		     	std::string semName = "/files/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client + "_followers.txt";
                        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

                        std::ofstream followerStream(followerFile, std::ios::app | std::ios::out | std::ios::in);
                        if (root.isMember(client))
                        {
                            for (const auto &follower : root[client])
                            {
                                if (!file_contains_user(followerFile, follower.asString()))
                                {
                                    followerStream << follower.asString() << std::endl;
                                }
                            }
                        }
                        sem_close(fileSem);
                    }
                }
            }
    }

    // for every client in your cluster, update all their followers' timeline files
    // by publishing your user's timeline file (or just the new updates in them)
    //  periodically to the message queue of the synchronizer responsible for that client
    void publishTimelines()
    {
        std::vector<std::string> users = get_all_users_func(synchID);
        for (const auto &client : users)
        {
            int clientId = std::stoi(client);
            int client_cluster = ((clientId - 1) % 3) + 1;
            // only do this for clients in your own cluster
            if (client_cluster != clusterID)
            {
                continue;
            }

            std::vector<std::string> timeline = get_tl_or_fl(synchID, clientId, true);
            std::vector<std::string> followers = getAllFollowers(clientId);
	    if (timeline.size() < 3 || followers.empty()) continue;
	    Json::Value timeline_msg;
	    int i = 0;
	    while (i < timeline.size() - 2) {
		    std::string line = timeline[i];
		    if (line[0] == 'T' && timeline[i+1].substr(21) == client) {
			    time_t t = getProtoTime(line);
			    std::string protime = ctime(&t);
			    time_t t_now = getTimeNow();
			    std::string nowtime = ctime(&t_now);
			    if (difftime(getTimeNow(), t) < 7) {
				timeline_msg[client].append(line);
				timeline_msg[client].append(timeline[i+1]);
				timeline_msg[client].append(timeline[i+2]);
			    }
			    i+=3;
		    } else i++;
	    }
	    Json::FastWriter writer;
	    std::string message = writer.write(timeline_msg);
	    log(INFO, "Publish TIMELINE message: " + message);
	    if (timeline_msg.isNull() || timeline_msg.empty()) continue;
	    publishMessage("ex_timeline", message, timeline_channel);
        }
    }

    // For each client in your cluster, consume messages from your timeline queue and modify your client's timeline files based on what the users they follow posted to their timeline
    void consumeTimelines(std::string message)
    {
        //std::string queueName = "synch" + std::to_string(synchID) + "_timeline_queue";
        //std::string message = consumeMessage(queueName, 1000, timeline_channel); // 1 second timeout
	log(INFO, "Consuming timelines: " + message);

        if (!message.empty())
        {
	    Json::Value root;
	    Json::Reader reader;
	    std::vector<std::string> allUsers = get_all_users_func(synchID);
	    if (reader.parse(message, root)) {
	    	for (const auto &client : allUsers) {
			int clientId = std::stoi(client);
            		int client_cluster = ((clientId - 1) % 3) + 1;
			if (client_cluster != clusterID) continue;

	                std::string timelineFile = "files/cluster_" + std::to_string(clusterID) 
				+ "/" + clusterSubdirectory + "/" + client + "_timeline.txt";
			std::string semName = "/files/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client + "_timeline.txt";
			sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
			std::ofstream timelineStream(timelineFile, std::ios::app | std::ios::out | std::ios::in);
			std::vector<std::string> followings = get_tl_or_fl(synchID, clientId, false);
			for (const auto &following : followings) {
				if (root.isMember(following)) {
					Json::ArrayIndex i = 0;
					while (i < root[following].size() - 2) {
						std::string t = root[following][i].asString();
						std::string u = root[following][i+1].asString();
						std::string w = root[following][i+2].asString();
						if (!file_contains_post(timelineFile, t, u, w)) {
							timelineStream << t << std::endl;
							timelineStream << u << std::endl;
							timelineStream << w << std::endl;
							timelineStream << std::endl;
						}
						i+=3;
					}
				}

			}
			sem_close(fileSem);
			timelineStream.close();

		}
	    }
            // consume the message from the queue and update the timeline file of the appropriate client with
            // the new updates to the timeline of the user it follows

            // YOUR CODE HERE
        }
    }

private:
    void updateAllUsersFile(const std::vector<std::string> &users)
    {
	std::string folder = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory;
	if (!fs::exists(folder)) { fs::create_directories(folder);}
        std::string usersFile = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
        std::string semName = "/files/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

        std::ofstream userStream(usersFile, std::ios::app | std::ios::out | std::ios::in);
        for (std::string user : users)
        {
            if (!file_contains_user(usersFile, user))
            {
                userStream << user << std::endl;
            }
        }
        sem_close(fileSem);
    }
};

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ);

class SynchServiceImpl final : public SynchService::Service
{
    // You do not need to modify this in any way
};

void RunServer(std::string coordIP, std::string coordPort, std::string port_no, int synchID, ServerInfo serverInfo)
{
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    log(INFO, "Starting synchronizer server at " + server_address);
    SynchServiceImpl service;
    // grpc::EnableDefaultHealthCheckService(true);
    // grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Initialize RabbitMQ connection
    SynchronizerRabbitMQ rabbitMQ("localhost", 5672, synchID);

    std::thread t1(run_synchronizer, coordIP, coordPort, port_no, synchID, std::ref(rabbitMQ));
    std::thread hb(Heartbeat, coordIP, coordPort, std::ref(serverInfo), synchID);

    // Create a consumer thread
    std::thread consumerThread([&rabbitMQ]()
                               {
        while (true) {
            //rabbitMQ.consumeUserLists();
            //rabbitMQ.consumeClientRelations();
            //rabbitMQ.consumeTimelines();
            rabbitMQ.consumeForAll();
	    std::this_thread::sleep_for(std::chrono::seconds(5));
            // you can modify this sleep period as per your choice
        } });
    server->Wait();
	t1.join();
    consumerThread.join();
}

int main(int argc, char **argv)
{
    int opt = 0;
    std::string coordIP;
    std::string coordPort;
    std::string port = "3029";

    while ((opt = getopt(argc, argv, "h:k:p:i:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            coordIP = optarg;
            break;
        case 'k':
            coordPort = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'i':
            synchID = std::stoi(optarg);
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }

    //std::string log_file_name = std::string("synchronizer-") + port;
    //google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Server starting...");

    coordAddr = coordIP + ":" + coordPort;
    clusterID = ((synchID - 1) % 3) + 1;
    ServerInfo serverInfo;
    serverInfo.set_hostname("localhost");
    serverInfo.set_port(port);
    serverInfo.set_type("synchronizer");
    serverInfo.set_serverid(synchID);
    serverInfo.set_clusterid(clusterID);
    //Heartbeat(coordIP, coordPort, serverInfo, synchID);

    RunServer(coordIP, coordPort, port, synchID, serverInfo);
    return 0;
}

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ)
{
    // setup coordinator stub
    std::string target_str = coordIP + ":" + coordPort;
    std::unique_ptr<CoordService::Stub> coord_stub_;
    coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())));

    ServerInfo msg;
    Confirmation c;

    msg.set_serverid(synchID);
    msg.set_hostname("127.0.0.1");
    msg.set_port(port);
    msg.set_type("follower");

    // TODO: begin synchronization process
    while (true)
    {
        // the synchronizers sync files every 5 seconds
        sleep(5);

        grpc::ClientContext context;
        ServerList followerServers;
        ID id;
        id.set_id(synchID);

        // making a request to the coordinator to see count of follower synchronizers
        Status status = coord_stub_->GetAllFollowerServers(&context, id, &followerServers);
	if (!status.ok()) {
		log(INFO, "GRPC failed when getting all follower synchronizers");
	} else {
		log(INFO, "Successfully getting all follower synchrnizers");
		//log(INFO, "List size: " + std::to_string(followerServers.serverid_size()));
	}

        std::vector<int> server_ids;
        std::vector<std::string> hosts, ports;
        for (std::string host : followerServers.hostname())
        {
            hosts.push_back(host);
        }
        for (std::string port : followerServers.port())
        {
            ports.push_back(port);
        }
        for (int serverid : followerServers.serverid())
        {
            server_ids.push_back(serverid);
        }

        // update the count of how many follower sychronizer processes the coordinator has registered
	total_number_of_registered_synchronizers = followerServers.serverid_size();
	for (int i = 0; i < total_number_of_registered_synchronizers; i++) {
		//std::string host_info = hosts[i] + ":" + ports[i];
		otherHosts.push_back(std::to_string(server_ids[i]));
		//log(INFO, "Synchronizer ID: " + std::to_string(server_ids[i]));
	}

        // below here, you run all the update functions that synchronize the state across all the clusters
        // make any modifications as necessary to satisfy the assignments requirements
        
	if (isMaster) {
		// Publish user list
        	rabbitMQ.publishUserList();

        	// Publish client relations
        	rabbitMQ.publishClientRelations();

        	// Publish timelines
        	rabbitMQ.publishTimelines();
	}
    }
    return;
}

std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
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

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID)
{
    // For the synchronizer, a single initial heartbeat RPC acts as an initialization method which
    // servers to register the synchronizer with the coordinator and determine whether it is a master
    bool first_fetch = true;
    while (true) {
    	//log(INFO, "Sending initial heartbeat to coordinator");
    	std::string coordinatorInfo = coordinatorIp + ":" + coordinatorPort;
   	std::unique_ptr<CoordService::Stub> stub = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(grpc::CreateChannel(coordinatorInfo, grpc::InsecureChannelCredentials())));

    	// send a heartbeat to the coordinator, which registers your follower synchronizer as either a master or a slave

    	// YOUR CODE HERE
    	ClientContext context;
    	Confirmation confirmation;
    	Status status = stub->Heartbeat(&context, serverInfo, &confirmation);
    	if (!status.ok() || !confirmation.status()) {
        	        LOG(ERROR) << "Cannot get confirmaton from Coordinator!";
                	exit(-1);
    	}
    	LOG(INFO) <<"Got confirmation from Coordinator";
    	if (confirmation.type() == "1") {
    		isMaster = true;
		if (first_fetch) {
			clusterSubdirectory = "1";
			first_fetch = false;
		}
    	} else {
		isMaster = false;
		if (first_fetch) {
			clusterSubdirectory = "2";
			first_fetch = false;
		}
    	}
    	std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

bool file_contains_user(std::string filename, std::string user)
{
    std::vector<std::string> users;
    // check username is valid
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    users = get_lines_from_file(filename);
    for (int i = 0; i < users.size(); i++)
    {
        // std::cout<<"Checking if "<<user<<" = "<<users[i]<<std::endl;
        if (user == users[i])
        {
            // std::cout<<"found"<<std::endl;
            sem_close(fileSem);
            return true;
        }
    }
    // std::cout<<"not found"<<std::endl;
    sem_close(fileSem);
    return false;
}

bool file_contains_post(std::string filename, std::string t, std::string u, std::string w) {
	std::vector<std::string> timeline;
	std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    	sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
	timeline = get_lines_from_file(filename);
	if (timeline.size() < 3) {
		sem_close(fileSem);
		return false;
	}
	int i = 0;
	while (i < timeline.size() - 2) {
		std::string line = timeline[i];
                if (line[0] == 'T') {
		    if (t == line && u == timeline[i+1] && w == timeline[i+2]) {
		    	sem_close(fileSem);
			return true;
		    }
		    i+=3;
                } else i++;

	}
	sem_close(fileSem);
	return false;

}

std::vector<std::string> get_all_users_func_pub(int synchID) {
    std::string clusterID = std::to_string(((synchID - 1) % 3) + 1);
    std::string master_users_file = "files/cluster_" + clusterID + "/1/all_users.txt";
    std::string slave_users_file = "files/cluster_" + clusterID + "/2/all_users.txt";
    const char* file_name = master_users_file.c_str();
    const char* file_slave = slave_users_file.c_str();
    struct stat fileStat;
    struct stat slaveStat;
                        if (stat(file_name, &fileStat) == 0) {
                                time_t m_time = fileStat.st_mtime;
                                if (difftime(getTimeNow(), m_time) < 10) {
                                        log(INFO, "Users list changed");
					return get_all_users_func(synchID);
                                }

                        }
			if (stat(file_slave, &slaveStat) == 0) {
				time_t t = slaveStat.st_mtime;
				if (difftime(getTimeNow(), t) < 10) {
					log(INFO, "Users list changed");
					return get_all_users_func(synchID);
				}
			}
	return std::vector<std::string>();
}

std::vector<std::string> get_all_users_func(int synchID)
{
    // read all_users file master and client for correct serverID
    // std::string master_users_file = "./master"+std::to_string(synchID)+"/all_users";
    // std::string slave_users_file = "./slave"+std::to_string(synchID)+"/all_users";
    std::string clusterID = std::to_string(((synchID - 1) % 3) + 1);
    std::string master_users_file = "files/cluster_" + clusterID + "/1/all_users.txt";
    std::string slave_users_file = "files/cluster_" + clusterID + "/2/all_users.txt";
    // take longest list and package into AllUsers message
    std::vector<std::string> master_user_list = get_lines_from_file(master_users_file);
    std::vector<std::string> slave_user_list = get_lines_from_file(slave_users_file);

    if (master_user_list.size() >= slave_user_list.size())
        return master_user_list;
    else
        return slave_user_list;
}

std::vector<std::string> get_tl_or_fl(int synchID, int clientID, bool tl)
{
    // std::string master_fn = "./master"+std::to_string(synchID)+"/"+std::to_string(clientID);
    // std::string slave_fn = "./slave"+std::to_string(synchID)+"/" + std::to_string(clientID);
    std::string master_fn = "files/cluster_" + std::to_string(clusterID) + "/1/" + std::to_string(clientID);
    std::string slave_fn = "files/cluster_" + std::to_string(clusterID) + "/2/" + std::to_string(clientID);
    if (tl)
    {
        master_fn.append("_timeline.txt");
        slave_fn.append("_timeline.txt");
    }
    else
    {
        master_fn.append("_following.txt");
        slave_fn.append("_following.txt");
    }

    std::vector<std::string> m = get_lines_from_file(master_fn);
    std::vector<std::string> s = get_lines_from_file(slave_fn);

    if (m.size() >= s.size())
    {
        return m;
    }
    else
    {
        return s;
    }
}

std::vector<std::string> getFollowersOfUser(int ID)
{
    std::vector<std::string> followers;
    std::string clientID = std::to_string(ID);
    std::vector<std::string> usersInCluster = get_all_users_func(synchID);

    for (auto userID : usersInCluster)
    { // Examine each user's following file
        std::string file = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + userID + "_following.txt";
        std::string semName = "/files/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + userID + "_following.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
        // std::cout << "Reading file " << file << std::endl;
        if (file_contains_user(file, clientID))
        {
            followers.push_back(userID);
        }
        sem_close(fileSem);
    }

    return followers;
}

std::vector<std::string> getFollowersOfUser_pub(int ID)
{
    std::vector<std::string> followers;
    std::string clientID = std::to_string(ID);
    std::vector<std::string> usersInCluster = get_all_users_func(synchID);

    for (auto userID : usersInCluster)
    { // Examine each user's following file
        std::string file = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + userID + "_following.txt";
	const char* file_name = file.c_str();
                        struct stat fileStat;
                        if (stat(file_name, &fileStat) == 0) {
                                time_t m_time = fileStat.st_mtime;
                                if (difftime(getTimeNow(), m_time) > 10) {
					continue;
                                }

                        }
        std::string semName = "/files/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + userID + "_following.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
        // std::cout << "Reading file " << file << std::endl;
        if (file_contains_user(file, clientID))
        {
            followers.push_back(userID);
        }
        sem_close(fileSem);
    }

    return followers;
}
std::vector<std::string> getAllFollowers(int ID) {
	std::string clientID = std::to_string(ID);
	std::string file = "files/cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + clientID + "_followers.txt";
	std::vector<std::string> followers = get_lines_from_file(file);
	return followers;
}
