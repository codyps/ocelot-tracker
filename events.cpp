#include "ocelot.h"
#include "config.h"
#include "db.h"
#include "worker.h"
#include "events.h"
#include "schedule.h"
#include <cerrno>

// chroot, setuid
#include <unistd.h>
#include <sys/types.h>

// inttostr
#include "misc_functions.h"

// Define the connection mother (first half) and connection middlemen (second half)

//TODO Better errors

//---------- Connection mother - spawns middlemen and lets them deal with the connection

connection_mother::connection_mother(worker * worker_obj, config * config_obj, mysql * db_obj) : sock(-1), work(worker_obj), conf(config_obj), db(db_obj) {
	open_connections = 0;
	opened_connections = 0;

	ai = getnetinfo(conf->host.c_str(), conf->port, SOCK_STREAM);
	if (!ai) {
		std::cerr << "getnetinfo failed.. " << errno << std::endl;
	}

	struct addrinfo *rp;
	for (rp = ai; rp != NULL; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype,
			      rp->ai_protocol);
		if (sock < 0) continue;

		int ret = 1;
		int err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				     &ret, 4);
		if (err != 0) {
			std::cout << "Could not reuse socket" << std::endl;
		}

		if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
			break;
		}
	}

	if (rp == NULL) {
		perror("Bind failed");
	}

	// Create libev event loop
	ev::io event_loop_watcher;

	event_loop_watcher.set<connection_mother, &connection_mother::handle_connect>(this);
	event_loop_watcher.start(sock, ev::READ);

	// Listen
	if(listen(sock, conf->max_connections) == -1) {
		perror("Listen failed");
	}

	// Set non-blocking
	int flags = fcntl(sock, F_GETFL);
	if(flags == -1) {
		std::cout << "Could not get socket flags" << std::endl;
	}
	if(fcntl(sock, F_SETFL, flags | O_NONBLOCK) == -1) {
		std::cout << "Could not set non-blocking" << std::endl;
	}

	// Create libev timer
	schedule timer(this, worker_obj, conf, db);

	schedule_event.set<schedule, &schedule::handle>(&timer);
	schedule_event.set(conf->schedule_interval, conf->schedule_interval); // After interval, every interval
	schedule_event.start();

	if (!getuid()) {
		std::cout << "Dropping root privileges..\n";
		if (chroot(".") < 0) {
			std::cerr << "Warning: couldn't chroot, " << strerror(errno) << std::endl;
		}
		if (!setuid(1000) < 0) {
			std::cerr << "Warning: couldn't setuid, " << strerror(errno) << std::endl;
		}
	}

	std::cout << "Sockets up, starting event loop!" << std::endl;
	ev_loop(ev_default_loop(0), 0);
}


void connection_mother::handle_connect(ev::io &watcher, int events_flags) {
	// Spawn a new middleman
	if(open_connections < conf->max_middlemen) {
		opened_connections++;
		new connection_middleman(sock, ai, work, this, conf);
	}
}

connection_mother::~connection_mother()
{
	close(sock);
}







//---------- Connection middlemen - these little guys live until their connection is closed

connection_middleman::connection_middleman(int &listen_socket, struct addrinfo* info, worker * new_work, connection_mother * mother_arg, config * config_obj) :
	conf(config_obj), mother (mother_arg), work(new_work), ai(info) {

	connect_sock = accept(listen_socket, ai->ai_addr, &ai->ai_addrlen);
	if(connect_sock == -1) {
		std::cout << "Accept failed, errno " << errno << ": " << strerror(errno) << std::endl;
		mother->increment_open_connections(); // destructor decrements open connections
		delete this;
		return;
	}

	// Set non-blocking
	int flags = fcntl(connect_sock, F_GETFL);
	if(flags == -1) {
		std::cout << "Could not get connect socket flags" << std::endl;
	}
	if(fcntl(connect_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
		std::cout << "Could not set non-blocking" << std::endl;
	}

	// Get their info
	client_addr = new sockaddr[ai->ai_addrlen];
	client_addr->sa_family = ai->ai_family;

	if(getpeername(connect_sock, client_addr, &ai->ai_addrlen) == -1) {
		//std::cout << "Could not get client info" << std::endl;
	}

	read_event.set<connection_middleman, &connection_middleman::handle_read>(this);
	read_event.start(connect_sock, ev::READ);

	// Let the socket timeout in timeout_interval seconds
	timeout_event.set<connection_middleman, &connection_middleman::handle_timeout>(this);
	timeout_event.set(conf->timeout_interval, 0);
	timeout_event.start();

	mother->increment_open_connections();
}

connection_middleman::~connection_middleman() {
	delete client_addr;
	close(connect_sock);
	mother->decrement_open_connections();
}

// Handler to read data from the socket, called by event loop when socket is readable
void connection_middleman::handle_read(ev::io &watcher, int events_flags) {
	read_event.stop();

	char buffer[conf->max_read_buffer + 1];
	memset(buffer, 0, conf->max_read_buffer + 1);
	int status = recv(connect_sock, &buffer, conf->max_read_buffer, 0);

	if(status == -1) {
		delete this;
		return;
	}

	std::string stringbuf = buffer;

	char* ip = get_ip_str(client_addr);
	std::string ip_str = ip;

	//--- CALL WORKER
	response = work->work(stringbuf, ip_str);

	delete ip;

	// Find out when the socket is writeable.
	// The loop in connection_mother will call handle_write when it is.
	write_event.set<connection_middleman, &connection_middleman::handle_write>(this);
	write_event.start(connect_sock, ev::WRITE);
}

// Handler to write data to the socket, called by event loop when socket is writeable
void connection_middleman::handle_write(ev::io &watcher, int events_flags) {
	write_event.stop();
	timeout_event.stop();
	std::string http_response = "HTTP/1.1 200\r\nServer: Ocelot 1.0\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n";
	http_response+=response;
	send(connect_sock, http_response.c_str(), http_response.size(), MSG_NOSIGNAL);
	delete this;
}

// After a middleman has been alive for timout_interval seconds, this is called
void connection_middleman::handle_timeout(ev::timer &watcher, int events_flags) {
	timeout_event.stop();
	read_event.stop();
	write_event.stop();
	delete this;
}

struct addrinfo* getnetinfo(const char* host, int port, int socktype)
{
	struct addrinfo hints;
	struct addrinfo* result;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = socktype;
	hints.ai_flags = AI_PASSIVE;
	std::string s_port = inttostr(port);

	int err;
	if ((err = getaddrinfo(host, s_port.c_str(), &hints, &result)) != 0) {
		std::cerr << "[getaddrinfo] " << gai_strerror(err) << std::endl;
	}
	return result;
}

/* http://www.retran.com/beej/sockaddr_inman.html */
char *get_ip_str(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		char* buf = new char[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
			buf, INET_ADDRSTRLEN);
                return buf;
	} else if (sa->sa_family == AF_INET6) {
		char* buf = new char[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
			buf, INET6_ADDRSTRLEN);
		return buf;
	}
	return NULL;
}
