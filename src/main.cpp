#define MAIN_PROXY_SQLITE3
#include <iostream>
#include <thread>
#include "btree_map.h"
#include "proxysql.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

//#define PROXYSQL_EXTERN
#include "cpp.h"

#include "ProxySQL_Statistics.hpp"
#include "MySQL_PreparedStatement.h"
#include "ProxySQL_Cluster.hpp"
#include "MySQL_Logger.hpp"
#include "SQLite3_Server.h"
#include "query_processor.h"
#include "MySQL_Authentication.hpp"
#include "MySQL_LDAP_Authentication.hpp"
#include "proxysql_restapi.h"
#include "Web_Interface.hpp"

#include <libdaemon/dfork.h>
#include <libdaemon/dsignal.h>
#include <libdaemon/dlog.h>
#include <libdaemon/dpid.h>
#include <libdaemon/dexec.h>
#include "ev.h"

#include "curl/curl.h"

#include <openssl/x509v3.h>

#include <sys/mman.h>

#include <uuid/uuid.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
extern "C" MySQL_LDAP_Authentication * create_MySQL_LDAP_Authentication_func() {
	return NULL;
}
*/





volatile create_MySQL_LDAP_Authentication_t * create_MySQL_LDAP_Authentication = NULL;
void * __mysql_ldap_auth;

volatile create_Web_Interface_t * create_Web_Interface = NULL;
void * __web_interface;


extern int ProxySQL_create_or_load_TLS(bool bootstrap, std::string& msg);

char *binary_sha1 = NULL;

// MariaDB client library redefines dlerror(), see https://mariadb.atlassian.net/browse/CONC-101
#ifdef dlerror
#undef dlerror
#endif

static pthread_mutex_t *lockarray;
#include <openssl/crypto.h>


// this fuction will be called as a deatached thread
static void * waitpid_thread(void *arg) {
	pid_t *cpid_ptr=(pid_t *)arg;
	int status;
	waitpid(*cpid_ptr, &status, 0);
	free(cpid_ptr);
	return NULL;
}



struct MemoryStruct {
	char *memory;
	size_t size;
};


static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;
	mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);
	assert(mem->memory);
	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;
	return realsize;
}


char * know_latest_version = NULL;
static unsigned int randID = 0;

static char * main_check_latest_version() {
	CURL *curl_handle;
	CURLcode res;
	struct MemoryStruct chunk;
	chunk.memory = (char *)malloc(1);
	chunk.size = 0;
	curl_global_init(CURL_GLOBAL_ALL);
	curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, "https://www.proxysql.com/latest");
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYSTATUS, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_RANGE, "0-31");
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

	string s = "proxysql-agent/";
	s += PROXYSQL_VERSION;
	if (binary_sha1) {
		s += " (" ;
			s+= binary_sha1;
		s += ")" ;
	}
	s += " " + std::to_string(randID);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, s.c_str());
	curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10);
	curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10);

	res = curl_easy_perform(curl_handle);

	if (res != CURLE_OK) {
		switch (res) {
			case CURLE_COULDNT_RESOLVE_HOST:
			case CURLE_COULDNT_CONNECT:
			case CURLE_OPERATION_TIMEDOUT:
				break;
			default:
				proxy_error("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
				break;
		}
		free(chunk.memory);
		chunk.memory = NULL;
	}
	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();
	return chunk.memory;
}


void * main_check_latest_version_thread(void *arg) {
	char * latest_version = main_check_latest_version();
	// we check for potential invalid data , see issue #4042 
	if (latest_version != NULL && strlen(latest_version) < 32) {
		if (
			(know_latest_version == NULL) // first check
			|| (strcmp(know_latest_version,latest_version)) // new version detected
		) {
			if (know_latest_version)
				free(know_latest_version);
			know_latest_version = strdup(latest_version);
			proxy_info("Latest ProxySQL version available: %s\n", latest_version);
		}
	}
	free(latest_version);
	return NULL;
}





// Note: if you are running ProxySQL under gdb, you may consider setting this
// variable immediately to 1
// Example:
// set disable_watchdog=1
volatile int disable_watchdog = 0;

void parent_open_error_log() {
	if (GloVars.global.foreground==false) {
		int outfd=0;
		int errfd=0;
		outfd=open(GloVars.errorlog, O_WRONLY | O_APPEND | O_CREAT , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (outfd>0) {
			dup2(outfd, STDOUT_FILENO);
			close(outfd);
		} else {
			proxy_error("Impossible to open file\n");
		}
		errfd=open(GloVars.errorlog, O_WRONLY | O_APPEND | O_CREAT , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		if (errfd>0) {
			dup2(errfd, STDERR_FILENO);
			close(errfd);
		} else {
			proxy_error("Impossible to open file\n");
		}
	}
}


void parent_close_error_log() {
	if (GloVars.global.foreground==false) {
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}
}

time_t laststart;
pid_t pid;

static const char * proxysql_pid_file() {
	static char fn[512];
	snprintf(fn, sizeof(fn), "%s", daemon_pid_file_ident);
	return fn;
}


/*struct cpu_timer
{
	~cpu_timer()
	{
		auto end = std::clock() ;
		std::cerr << double( end - begin ) / CLOCKS_PER_SEC << " secs.\n" ;
	};
	const std::clock_t begin = std::clock() ;
};
*/
struct cpu_timer
{
	cpu_timer() {
		begin = monotonic_time();
	}
	~cpu_timer()
	{
		unsigned long long end = monotonic_time();
#ifdef DEBUG
		std::cerr << double( end - begin ) / 1000000 << " secs.\n" ;
#endif /* DEBUG */
		begin=end-begin; // here only to make compiler happy
	};
	unsigned long long begin;
};


static unsigned long thread_id(void) {
	unsigned long ret;
	ret = (unsigned long)pthread_self();
	return ret;
}

static void init_locks(void) {
	int i;
	lockarray = (pthread_mutex_t *)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
	for(i = 0; i<CRYPTO_num_locks(); i++) {
		pthread_mutex_init(&(lockarray[i]), NULL);
	}
	CRYPTO_set_id_callback((unsigned long (*)())thread_id);
	// deprecated
	//CRYPTO_set_locking_callback((void (*)(int, int, const char *, int))lock_callback);
}




void ProxySQL_Main_init_SSL_module() {
	int rc = SSL_library_init();
	if (rc==0) {
		proxy_error("%s\n", SSL_alert_desc_string_long(rc));
	}
	init_locks();
	proxy_info("Using OpenSSL version: %s\n", OpenSSL_version(OPENSSL_VERSION));
	SSL_METHOD *ssl_method;
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();
	//ssl_method = (SSL_METHOD *)TLSv1_server_method();
	//ssl_method = (SSL_METHOD *)SSLv23_server_method();
	ssl_method = (SSL_METHOD *)TLS_server_method();
	GloVars.global.ssl_ctx = SSL_CTX_new(ssl_method);
	if (GloVars.global.ssl_ctx==NULL)	{
		ERR_print_errors_fp(stderr);
		proxy_error("Unable to initialize SSL. Shutting down...\n");
		exit(EXIT_SUCCESS); // we exit gracefully to not be restarted
	}
	if (!SSL_CTX_set_min_proto_version(GloVars.global.ssl_ctx,TLS1_VERSION)) {
		proxy_error("Unable to initialize SSL. SSL_set_min_proto_version failed. Shutting down...\n");
		exit(EXIT_SUCCESS); // we exit gracefully to not be restarted
	}
	//SSL_CTX_set_options(GloVars.global.ssl_ctx, SSL_OP_NO_SSLv3); // no necessary, because of previous SSL_CTX_set_min_proto_version
#ifdef DEBUG
	{
		STACK_OF(SSL_CIPHER) *ciphers;
		ciphers = SSL_CTX_get_ciphers(GloVars.global.ssl_ctx);
		fprintf(stderr,"List of cipher avaiable:\n");
		if (ciphers) {
			int num = sk_SSL_CIPHER_num(ciphers);
			char buf[130];
			for(int i = 0; i < num; i++){
				const SSL_CIPHER *cipher = sk_SSL_CIPHER_value(ciphers, i);
				fprintf(stderr,"%s:  %s", SSL_CIPHER_get_name(cipher), SSL_CIPHER_description(cipher, buf, 128));
			}
		}
		fprintf(stderr,"\n");
	}
#endif
	std::string msg = "";
	ProxySQL_create_or_load_TLS(true, msg);
}


/*
void example_listern() {
// few examples tests to demonstrate the ability to add and remove listeners at runtime
	GloMTH->listener_add((char *)"0.0.0.0:6033");
	sleep(3);
	GloMTH->listener_add((char *)"127.0.0.1:5033");
	sleep(3);
	GloMTH->listener_add((char *)"127.0.0.2:5033");
	sleep(3);
	GloMTH->listener_add((char *)"/tmp/proxysql.sock");
	for (int t=0; t<10; t++) {
		GloMTH->listener_add((char *)"127.0.0.1",7000+t);
		sleep(3);
	}

	GloMTH->listener_del((char *)"0.0.0.0:6033");
	sleep(3);
	GloMTH->listener_del((char *)"127.0.0.1:5033");
	sleep(3);
	GloMTH->listener_del((char *)"127.0.0.2:5033");
	sleep(3);
	GloMTH->listener_del((char *)"/tmp/proxysql.sock");
}
*/




void * __qc;
void * __mysql_thread;
void * __mysql_threads_handler;
void * __query_processor;
//void * __mysql_auth;



using namespace std;


//__cmd_proxysql_config_file=NULL;
#define MAX_EVENTS 100

static volatile int load_;

//__thread l_sfp *__thr_sfp=NULL;
//#ifdef DEBUG
//const char *malloc_conf = "xmalloc:true,lg_tcache_max:16,purge:decay,junk:true,tcache:false";
//#else
//const char *malloc_conf = "xmalloc:true,lg_tcache_max:16,purge:decay";
#ifndef __FreeBSD__
const char *malloc_conf = "xmalloc:true,lg_tcache_max:16,prof:true,prof_leak:true,lg_prof_sample:20,lg_prof_interval:30,prof_active:false";
#endif
//#endif /* DEBUG */
//const char *malloc_conf = "prof_leak:true,lg_prof_sample:0,prof_final:true,xmalloc:true,lg_tcache_max:16";

int listen_fd;
int socket_fd;


Query_Cache *GloQC;
MySQL_Authentication *GloMyAuth;
MySQL_LDAP_Authentication *GloMyLdapAuth;
#ifdef PROXYSQLCLICKHOUSE
ClickHouse_Authentication *GloClickHouseAuth;
#endif /* PROXYSQLCLICKHOUSE */
Query_Processor *GloQPro;
ProxySQL_Admin *GloAdmin;
MySQL_Threads_Handler *GloMTH = NULL;
Web_Interface *GloWebInterface;
MySQL_STMT_Manager_v14 *GloMyStmt;

MySQL_Monitor *GloMyMon;
std::thread *MyMon_thread = NULL;

MySQL_Logger *GloMyLogger;
MySQL_Variables mysql_variables;

SQLite3_Server *GloSQLite3Server;
#ifdef PROXYSQLCLICKHOUSE
ClickHouse_Server *GloClickHouseServer;
#endif /* PROXYSQLCLICKHOUSE */


ProxySQL_Cluster *GloProxyCluster = NULL;

ProxySQL_Statistics *GloProxyStats = NULL;


void * mysql_worker_thread_func(void *arg) {

//	__thr_sfp=l_mem_init();

	pthread_attr_t thread_attr;
	size_t tmp_stack_size=0;
	if (!pthread_attr_init(&thread_attr)) {
		if (!pthread_attr_getstacksize(&thread_attr , &tmp_stack_size )) {
			__sync_fetch_and_add(&GloVars.statuses.stack_memory_mysql_threads,tmp_stack_size);
		}
	}

	proxysql_mysql_thread_t *mysql_thread=(proxysql_mysql_thread_t *)arg;
	MySQL_Thread *worker = new MySQL_Thread();
	mysql_thread->worker=worker;
	worker->init();
//	worker->poll_listener_add(listen_fd);
//	worker->poll_listener_add(socket_fd);
	__sync_fetch_and_sub(&load_,1);
	do { usleep(50); } while (load_);

	worker->run();
	//delete worker;
	delete worker;
	mysql_thread->worker=NULL;
//	l_mem_destroy(__thr_sfp);
	__sync_fetch_and_sub(&GloVars.statuses.stack_memory_mysql_threads,tmp_stack_size);
	return NULL;
}

#ifdef IDLE_THREADS
void * mysql_worker_thread_func_idles(void *arg) {

	pthread_attr_t thread_attr;
	size_t tmp_stack_size=0;
	if (!pthread_attr_init(&thread_attr)) {
		if (!pthread_attr_getstacksize(&thread_attr , &tmp_stack_size )) {
			__sync_fetch_and_add(&GloVars.statuses.stack_memory_mysql_threads,tmp_stack_size);
		}
	}

//	__thr_sfp=l_mem_init();
	proxysql_mysql_thread_t *mysql_thread=(proxysql_mysql_thread_t *)arg;
	MySQL_Thread *worker = new MySQL_Thread();
	mysql_thread->worker=worker;
	worker->epoll_thread=true;
	worker->init();
//	worker->poll_listener_add(listen_fd);
//	worker->poll_listener_add(socket_fd);
	__sync_fetch_and_sub(&load_,1);
	do { usleep(50); } while (load_);

	worker->run();
	//delete worker;
	delete worker;
//	l_mem_destroy(__thr_sfp);

	__sync_fetch_and_sub(&GloVars.statuses.stack_memory_mysql_threads,tmp_stack_size);

	return NULL;
}
#endif // IDLE_THREADS

void * mysql_shared_query_cache_funct(void *arg) {
	GloQC->purgeHash_thread(NULL);
	return NULL;
}


void ProxySQL_Main_process_global_variables(int argc, const char **argv) {
	GloVars.errorlog = NULL;
	GloVars.pid = NULL;
	GloVars.parse(argc,argv);
	GloVars.process_opts_pre();
	GloVars.restart_on_missing_heartbeats = 10; // default
	// alwasy try to open a config file
	if (GloVars.confFile->OpenFile(GloVars.config_file) == true) {
		GloVars.configfile_open=true;
		proxy_info("Using config file %s\n", GloVars.config_file);
		const Setting& root = GloVars.confFile->cfg.getRoot();
		if (root.exists("restart_on_missing_heartbeats")==true) {
			// restart_on_missing_heartbeats datadir from config file
			int restart_on_missing_heartbeats;
			bool rc;
			rc=root.lookupValue("restart_on_missing_heartbeats", restart_on_missing_heartbeats);
			if (rc==true) {
				GloVars.restart_on_missing_heartbeats=restart_on_missing_heartbeats;
			}
		}
		if (root.exists("execute_on_exit_failure")==true) {
			// restart_on_missing_heartbeats datadir from config file
			string execute_on_exit_failure;
			bool rc;
			rc=root.lookupValue("execute_on_exit_failure", execute_on_exit_failure);
			if (rc==true) {
				GloVars.execute_on_exit_failure=strdup(execute_on_exit_failure.c_str());
			}
		}
		if (root.exists("errorlog")==true) {
			// restart_on_missing_heartbeats datadir from config file
			string errorlog_path;
			bool rc;
			rc=root.lookupValue("errorlog", errorlog_path);
			if (rc==true) {
				GloVars.errorlog = strdup(errorlog_path.c_str());
			}
		}
		if (root.exists("uuid")==true) {
			string uuid;
			bool rc;
			rc=root.lookupValue("uuid", uuid);
			if (rc==true) {
				uuid_t uu;
				if (uuid_parse(uuid.c_str(), uu)==0) {
					if (GloVars.uuid == NULL) {
						// it is not set yet, that means it wasn't specified on the cmdline
						GloVars.uuid = strdup(uuid.c_str());
					}
				} else {
					proxy_error("The config file is configured with an invalid UUID: %s\n", uuid.c_str());
				}
			}
		}
		// if cluster_sync_interfaces is true, interfaces variables are synced too
		if (root.exists("cluster_sync_interfaces")==true) {
			bool value_bool;
			bool rc;
			rc=root.lookupValue("cluster_sync_interfaces", value_bool);
			if (rc==true) {
				GloVars.cluster_sync_interfaces=value_bool;
			} else {
				proxy_error("The config file is configured with an invalid cluster_sync_interfaces\n");
			}
		}
		if (root.exists("pidfile")==true) {
			string pidfile_path;
			bool rc;
			rc=root.lookupValue("pidfile", pidfile_path);
			if (rc==true) {
				GloVars.pid = strdup(pidfile_path.c_str());
      }
    }
		if (root.exists("sqlite3_plugin")==true) {
			string sqlite3_plugin;
			bool rc;
			rc=root.lookupValue("sqlite3_plugin", sqlite3_plugin);
			if (rc==true) {
				GloVars.sqlite3_plugin=strdup(sqlite3_plugin.c_str());
			}
		}
		if (root.exists("web_interface_plugin")==true) {
			string web_interface_plugin;
			bool rc;
			rc=root.lookupValue("web_interface_plugin", web_interface_plugin);
			if (rc==true) {
				GloVars.web_interface_plugin=strdup(web_interface_plugin.c_str());
			}
		}
		if (root.exists("ldap_auth_plugin")==true) {
			string ldap_auth_plugin;
			bool rc;
			rc=root.lookupValue("ldap_auth_plugin", ldap_auth_plugin);
			if (rc==true) {
				GloVars.ldap_auth_plugin=strdup(ldap_auth_plugin.c_str());
			}
		}
	} else {
		proxy_warning("Unable to open config file %s\n", GloVars.config_file); // issue #705
		if (GloVars.__cmd_proxysql_config_file) {
			proxy_error("Unable to open config file %s specified in the command line. Aborting!\n", GloVars.config_file);
			exit(EXIT_SUCCESS); // we exit gracefully to avoid restart
		}
	}
	char *t=getcwd(NULL, 512);
	if (GloVars.__cmd_proxysql_datadir==NULL) {
		// datadir was not specified , try to read config file
		if (GloVars.configfile_open==true) {
			const Setting& root = GloVars.confFile->cfg.getRoot();
			if (root.exists("datadir")==true) {
				// reading datadir from config file
				std::string datadir;
				bool rc;
				rc=root.lookupValue("datadir", datadir);
				if (rc==true) {
					GloVars.datadir=strdup(datadir.c_str());
				} else {
					GloVars.datadir=strdup(t);
				}
			} else {
				// datadir was not specified in config file
				GloVars.datadir=strdup(t);
			}
			if (root.exists("restart_on_missing_heartbeats")==true) {
				// restart_on_missing_heartbeats datadir from config file
				int restart_on_missing_heartbeats;
				bool rc;
				rc=root.lookupValue("restart_on_missing_heartbeats", restart_on_missing_heartbeats);
				if (rc==true) {
					GloVars.restart_on_missing_heartbeats=restart_on_missing_heartbeats;
				} else {
					GloVars.restart_on_missing_heartbeats = 10; // default
				}
			} else {
				// restart_on_missing_heartbeats was not specified in config file
				GloVars.restart_on_missing_heartbeats = 10; // default
			}
		} else {
			// config file not readable
			GloVars.datadir=strdup(t);
			std::cerr << "[Warning]: Cannot open any default config file . Using default datadir in current working directory " << GloVars.datadir << endl;
		}
	} else {
		GloVars.datadir=GloVars.__cmd_proxysql_datadir;
	}
	free(t);

	GloVars.admindb=(char *)malloc(strlen(GloVars.datadir)+strlen((char *)"proxysql.db")+2);
	sprintf(GloVars.admindb,"%s/%s",GloVars.datadir, (char *)"proxysql.db");

	GloVars.sqlite3serverdb=(char *)malloc(strlen(GloVars.datadir)+strlen((char *)"sqlite3server.db")+2);
	sprintf(GloVars.sqlite3serverdb,"%s/%s",GloVars.datadir, (char *)"sqlite3server.db");

	GloVars.statsdb_disk=(char *)malloc(strlen(GloVars.datadir)+strlen((char *)"proxysql_stats.db")+2);
	sprintf(GloVars.statsdb_disk,"%s/%s",GloVars.datadir, (char *)"proxysql_stats.db");

	if (GloVars.errorlog == NULL) {
		GloVars.errorlog=(char *)malloc(strlen(GloVars.datadir)+strlen((char *)"proxysql.log")+2);
		sprintf(GloVars.errorlog,"%s/%s",GloVars.datadir, (char *)"proxysql.log");
	}

	if (GloVars.pid == NULL) {
		GloVars.pid=(char *)malloc(strlen(GloVars.datadir)+strlen((char *)"proxysql.pid")+2);
		sprintf(GloVars.pid,"%s/%s",GloVars.datadir, (char *)"proxysql.pid");
	}

	if (GloVars.__cmd_proxysql_initial==true) {
		std::cerr << "Renaming database file " << GloVars.admindb << endl;
		char *newpath=(char *)malloc(strlen(GloVars.admindb)+8);
		sprintf(newpath,"%s.bak",GloVars.admindb);
		rename(GloVars.admindb,newpath);	// FIXME: should we check return value, or ignore whatever it successed or not?
	}

	GloVars.confFile->ReadGlobals();
	GloVars.process_opts_post();
}

void ProxySQL_Main_init_main_modules() {
	GloQC=NULL;
	GloQPro=NULL;
	GloMTH=NULL;
	GloMyAuth=NULL;
#ifdef PROXYSQLCLICKHOUSE
	GloClickHouseAuth=NULL;
#endif /* PROXYSQLCLICKHOUSE */
	GloMyMon=NULL;
	GloMyLogger=NULL;
	GloMyStmt=NULL;

	// initialize libev
	if (!ev_default_loop (EVBACKEND_POLL | EVFLAG_NOENV)) {
		fprintf(stderr,"could not initialise libev");
		exit(EXIT_FAILURE);
	}

	MyHGM=new MySQL_HostGroups_Manager();
	MyHGM->init();
	MySQL_Threads_Handler * _tmp_GloMTH = NULL;
	_tmp_GloMTH=new MySQL_Threads_Handler();
	GloMTH = _tmp_GloMTH;
	GloMyLogger = new MySQL_Logger();
	GloMyLogger->print_version();
	GloMyStmt=new MySQL_STMT_Manager_v14();
}


void ProxySQL_Main_init_Admin_module() {
	// cluster module needs to be initialized before
	GloProxyCluster = new ProxySQL_Cluster();
	GloProxyCluster->init();
	GloProxyCluster->print_version();
	GloProxyStats = new ProxySQL_Statistics();
	//GloProxyStats->init();
	GloProxyStats->print_version();
	GloAdmin = new ProxySQL_Admin();
	GloAdmin->init();
	GloAdmin->print_version();
	if (binary_sha1) {
		proxy_info("ProxySQL SHA1 checksum: %s\n", binary_sha1);
	}
}

void ProxySQL_Main_init_Auth_module() {
	GloMyAuth = new MySQL_Authentication();
	GloMyAuth->print_version();
	GloAdmin->init_users();
	//GloMyLdapAuth = create_MySQL_LDAP_Authentication();
	if (GloMyLdapAuth) {
		GloMyLdapAuth->print_version();
	}
}

void ProxySQL_Main_init_Query_module() {
	GloQPro = new Query_Processor();
	GloQPro->print_version();
	GloAdmin->init_mysql_query_rules();
	GloAdmin->init_mysql_firewall();
//	if (GloWebInterface) {
//		GloWebInterface->print_version();
//	}
}

void ProxySQL_Main_init_MySQL_Threads_Handler_module() {
	unsigned int i;
	GloMTH->init();
	load_ = 1;
	load_ += GloMTH->num_threads;
#ifdef IDLE_THREADS
	if (GloVars.global.idle_threads) {
		load_ += GloMTH->num_threads;
	} else {
		proxy_warning("proxysql instance running without --idle-threads : most workloads benefit from this option\n");
		proxy_warning("proxysql instance running without --idle-threads : enabling it can potentially improve performance\n");
	}
#endif // IDLE_THREADS
	for (i=0; i<GloMTH->num_threads; i++) {
		GloMTH->create_thread(i,mysql_worker_thread_func, false);
#ifdef IDLE_THREADS
		if (GloVars.global.idle_threads) {
			GloMTH->create_thread(i,mysql_worker_thread_func_idles, true);
		}
#endif // IDLE_THREADS
	}
}

void ProxySQL_Main_init_Query_Cache_module() {
	GloQC = new Query_Cache();
	GloQC->print_version();
	pthread_create(&GloQC->purge_thread_id, NULL, mysql_shared_query_cache_funct , NULL);
}

void ProxySQL_Main_init_MySQL_Monitor_module() {
	// start MySQL_Monitor
//	GloMyMon = new MySQL_Monitor();
	if (MyMon_thread == NULL) { // only if not created yet
		MyMon_thread = new std::thread(&MySQL_Monitor::run,GloMyMon);
		GloMyMon->print_version();
	}
}


void ProxySQL_Main_init_SQLite3Server() {
	// start SQLite3Server
	GloSQLite3Server = new SQLite3_Server();
	GloSQLite3Server->init();
	// NOTE: Always perform the 'load_*_to_runtime' after module start, otherwise values won't be properly
	// loaded from disk at ProxySQL startup.
	GloAdmin->load_sqliteserver_variables_to_runtime();
	GloAdmin->init_sqliteserver_variables();
	GloSQLite3Server->print_version();
}
#ifdef PROXYSQLCLICKHOUSE
void ProxySQL_Main_init_ClickHouseServer() {
	// start SQServer
	GloClickHouseServer = new ClickHouse_Server();
	GloClickHouseServer->init();
	// NOTE: Always perform the 'load_*_to_runtime' after module start, otherwise values won't be properly
	// loaded from disk at ProxySQL startup.
	GloAdmin->load_clickhouse_variables_to_runtime();
	GloAdmin->init_clickhouse_variables();
	GloClickHouseServer->print_version();
	GloClickHouseAuth = new ClickHouse_Authentication();
	GloClickHouseAuth->print_version();
	GloAdmin->init_clickhouse_users();
}
#endif /* PROXYSQLCLICKHOUSE */

void ProxySQL_Main_join_all_threads() {
	cpu_timer t;
	if (GloMTH) {
		cpu_timer t;
		GloMTH->shutdown_threads();
#ifdef DEBUG
		std::cerr << "GloMTH joined in ";
#endif
	}
	if (GloQC) {
		GloQC->shutdown=1;
	}

	if (GloMyMon) {
		GloMyMon->shutdown=true;
	}

	// join GloMyMon thread
	if (GloMyMon && MyMon_thread) {
		cpu_timer t;
		MyMon_thread->join();
		delete MyMon_thread;
		MyMon_thread = NULL;
#ifdef DEBUG
		std::cerr << "GloMyMon joined in ";
#endif
	}

	// join GloQC thread
	if (GloQC) {
		cpu_timer t;
		pthread_join(GloQC->purge_thread_id, NULL);
#ifdef DEBUG
		std::cerr << "GloQC joined in ";
#endif
	}
#ifdef DEBUG
	std::cerr << "All threads joined in ";
#endif
}

void ProxySQL_Main_shutdown_all_modules() {
	if (GloMyMon) {
		cpu_timer t;
		delete GloMyMon;
		GloMyMon=NULL;
#ifdef DEBUG
		std::cerr << "GloMyMon shutdown in ";
#endif
	}

	if (GloQC) {
		cpu_timer t;
		delete GloQC;
		GloQC=NULL;
#ifdef DEBUG
		std::cerr << "GloQC shutdown in ";
#endif
	}
	if (GloQPro) {
		cpu_timer t;
		delete GloQPro;
		GloQPro=NULL;
#ifdef DEBUG
		std::cerr << "GloQPro shutdown in ";
#endif
	}
#ifdef PROXYSQLCLICKHOUSE
	if (GloClickHouseAuth) {
		cpu_timer t;
		delete GloClickHouseAuth;
		GloClickHouseAuth=NULL;
#ifdef DEBUG
		std::cerr << "GloClickHouseAuth shutdown in ";
#endif
	}
	if (GloClickHouseServer) {
		cpu_timer t;
		delete GloClickHouseServer;
		GloClickHouseServer=NULL;
#ifdef DEBUG
		std::cerr << "GloClickHouseServer shutdown in ";
#endif
	}
#endif /* PROXYSQLCLICKHOUSE */
	if (GloSQLite3Server) {
		cpu_timer t;
		delete GloSQLite3Server;
		GloSQLite3Server=NULL;
#ifdef DEBUG
		std::cerr << "GloSQLite3Server shutdown in ";
#endif
	}
	if (GloMyAuth) {
		cpu_timer t;
		delete GloMyAuth;
		GloMyAuth=NULL;
#ifdef DEBUG
		std::cerr << "GloMyAuth shutdown in ";
#endif
	}
	if (GloMTH) {
		cpu_timer t;
		pthread_mutex_lock(&GloVars.global.ext_glomth_mutex);
		delete GloMTH;
		GloMTH=NULL;
		pthread_mutex_unlock(&GloVars.global.ext_glomth_mutex);
#ifdef DEBUG
		std::cerr << "GloMTH shutdown in ";
#endif
	}
	if (GloMyLogger) {
		cpu_timer t;
		delete GloMyLogger;
		GloMyLogger=NULL;
#ifdef DEBUG
		std::cerr << "GloMyLogger shutdown in ";
#endif
	}

	{
#ifdef TEST_WITHASAN
		pthread_mutex_lock(&GloAdmin->sql_query_global_mutex);
#endif
		cpu_timer t;
		delete GloAdmin;
#ifdef DEBUG
		std::cerr << "GloAdmin shutdown in ";
#endif
	}
	{
		cpu_timer t;
		MyHGM->shutdown();
		delete MyHGM;
#ifdef DEBUG
		std::cerr << "GloHGM shutdown in ";
#endif
	}
	if (GloMyStmt) {
		delete GloMyStmt;
		GloMyStmt=NULL;
	}
}

void ProxySQL_Main_init() {
#ifdef DEBUG
	GloVars.global.gdbg=false;
	glovars.has_debug=true;
#else
	glovars.has_debug=false;
#endif /* DEBUG */
//	__thr_sfp=l_mem_init();
	proxysql_init_debug_prometheus_metrics();
}





static void LoadPlugins() {
	GloMyLdapAuth = NULL;
	SQLite3DB::LoadPlugin(GloVars.sqlite3_plugin);
	if (GloVars.web_interface_plugin) {
		dlerror();
		char * dlsym_error = NULL;
		dlerror();
		dlsym_error=NULL;
		__web_interface = dlopen(GloVars.web_interface_plugin, RTLD_NOW);
		if (!__web_interface) {
			cerr << "Cannot load library: " << dlerror() << '\n';
			exit(EXIT_FAILURE);
		} else {
			dlerror();
			create_Web_Interface = (create_Web_Interface_t *) dlsym(__web_interface, "create_Web_Interface_func");
			dlsym_error = dlerror();
			if (dlsym_error!=NULL) {
				cerr << "Cannot load symbol create_Web_Interface: " << dlsym_error << '\n';
				exit(EXIT_FAILURE);
			}
		}
		if (__web_interface==NULL || dlsym_error) {
			proxy_error("Unable to load Web_Interface from %s\n", GloVars.web_interface_plugin);
			exit(EXIT_FAILURE);
		} else {
			GloWebInterface = create_Web_Interface();
			if (GloWebInterface) {
				//GloAdmin->init_WebInterfacePlugin();
				//GloAdmin->load_ldap_variables_to_runtime();
			}
		}
	}
	if (GloVars.ldap_auth_plugin) {
		dlerror();
		char * dlsym_error = NULL;
		dlerror();
		dlsym_error=NULL;
		__mysql_ldap_auth = dlopen(GloVars.ldap_auth_plugin, RTLD_NOW);
		if (!__mysql_ldap_auth) {
			cerr << "Cannot load library: " << dlerror() << '\n';
			exit(EXIT_FAILURE);
		} else {
			dlerror();
			create_MySQL_LDAP_Authentication = (create_MySQL_LDAP_Authentication_t *) dlsym(__mysql_ldap_auth, "create_MySQL_LDAP_Authentication_func");
			dlsym_error = dlerror();
			if (dlsym_error!=NULL) {
				cerr << "Cannot load symbol create_MySQL_LDAP_Authentication: " << dlsym_error << '\n';
				exit(EXIT_FAILURE);
			}
		}
		if (__mysql_ldap_auth==NULL || dlsym_error) {
			proxy_error("Unable to load MySQL_LDAP_Authentication from %s\n", GloVars.ldap_auth_plugin);
			exit(EXIT_FAILURE);
		} else {
			GloMyLdapAuth = create_MySQL_LDAP_Authentication();
			// we are removing this from here, and copying in
			//     ProxySQL_Main_init_phase2___not_started
			// the keep record of these two lines to make sure we don't
			// do a similar mistakes with other plugins
			//
			//if (GloMyLdapAuth) {
			//	GloAdmin->init_ldap();
			//	GloAdmin->load_ldap_variables_to_runtime();
			//}
		}
	}
}

/**
 * @brief Unloads all the plugins that hold some resources that
 *  need to be deallocated.
 */
void UnloadPlugins() {
	if (GloWebInterface) {
		GloWebInterface->stop();
	}
}

void ProxySQL_Main_init_phase2___not_started() {
	LoadPlugins();

	ProxySQL_Main_init_main_modules();
	ProxySQL_Main_init_Admin_module();
	GloMTH->print_version();

	{
		cpu_timer t;
		GloMyLogger->events_set_datadir(GloVars.datadir);
		GloMyLogger->audit_set_datadir(GloVars.datadir);
#ifdef DEBUG
		std::cerr << "Main phase3 : GloMyLogger initialized in ";
#endif
	}
	if (GloVars.configfile_open) {
		GloVars.confFile->CloseFile();
	}

	if (GloMyLdapAuth) {
		GloAdmin->load_ldap_variables_to_runtime();
	}

	ProxySQL_Main_init_Auth_module();

	if (GloVars.global.nostart) {
		pthread_mutex_lock(&GloVars.global.start_mutex);
	}
}


void ProxySQL_Main_init_phase3___start_all() {

	{
		cpu_timer t;
		GloMyLogger->events_set_datadir(GloVars.datadir);
		GloMyLogger->audit_set_datadir(GloVars.datadir);
#ifdef DEBUG
		std::cerr << "Main phase3 : GloMyLogger initialized in ";
#endif
	}
	// Initialized monitor, no matter if it will be started or not
	GloMyMon = new MySQL_Monitor();
	// load all mysql servers to GloHGH
	{
		cpu_timer t;
		GloAdmin->init_mysql_servers();
		GloAdmin->init_proxysql_servers();
		GloAdmin->load_scheduler_to_runtime();
		GloAdmin->proxysql_restapi().load_restapi_to_runtime();
#ifdef DEBUG
		std::cerr << "Main phase3 : GloAdmin initialized in ";
#endif
	}
	{
		cpu_timer t;
		ProxySQL_Main_init_Query_module();
#ifdef DEBUG
		std::cerr << "Main phase3 : Query Processor initialized in ";
#endif
	}
	{
		cpu_timer t;
		ProxySQL_Main_init_MySQL_Threads_Handler_module();
#ifdef DEBUG
		std::cerr << "Main phase3 : MySQL Threads Handler initialized in ";
#endif
	}
	{
		cpu_timer t;
		ProxySQL_Main_init_Query_Cache_module();
#ifdef DEBUG
		std::cerr << "Main phase3 : Query Cache initialized in ";
#endif
	}

	do { /* nothing */
#ifdef DEBUG
		usleep(5+rand()%10);
#endif
	} while (load_ != 1);
	load_ = 0;
	__sync_fetch_and_add(&GloMTH->status_variables.threads_initialized, 1);

	{
		cpu_timer t;
		GloMTH->start_listeners();
#ifdef DEBUG
		std::cerr << "Main phase3 : MySQL Threads Handler listeners started in ";
#endif
	}
	if ( GloVars.global.sqlite3_server == true ) {
		cpu_timer t;
		ProxySQL_Main_init_SQLite3Server();
		sleep(1);
#ifdef DEBUG
		std::cerr << "Main phase3 : SQLite3 Server initialized in ";
#endif
	}
	if (GloVars.global.monitor==true)
		{
			cpu_timer t;
			ProxySQL_Main_init_MySQL_Monitor_module();
#ifdef DEBUG
			std::cerr << "Main phase3 : MySQL Monitor initialized in ";
#endif
		}
#ifdef PROXYSQLCLICKHOUSE
	if ( GloVars.global.clickhouse_server == true ) {
		cpu_timer t;
		ProxySQL_Main_init_ClickHouseServer();
#ifdef DEBUG
		std::cerr << "Main phase3 : ClickHouse Server initialized in ";
#endif
	}
#endif /* PROXYSQLCLICKHOUSE */

	// LDAP
	if (GloMyLdapAuth) {
		GloAdmin->init_ldap_variables();
	}
}



void ProxySQL_Main_init_phase4___shutdown() {
	cpu_timer t;
	ProxySQL_Main_join_all_threads();

	//write(GloAdmin->pipefd[1], &GloAdmin->pipefd[1], 1);	// write a random byte
	if (GloVars.global.nostart) {
		pthread_mutex_unlock(&GloVars.global.start_mutex);
	}

	ProxySQL_Main_shutdown_all_modules();
#ifdef DEBUG
	std::cerr << "Main init phase4 shutdown completed in ";
#endif
}


void ProxySQL_daemonize_phase1(char *argv0) {
	int rc;
	daemon_pid_file_ident=GloVars.pid;
	daemon_log_ident=daemon_ident_from_argv0(argv0);
	rc=chdir(GloVars.datadir);
	if (rc) {
		daemon_log(LOG_ERR, "Could not chdir into datadir: %s . Error: %s", GloVars.datadir, strerror(errno));
		exit(EXIT_FAILURE);
	}
	daemon_pid_file_proc=proxysql_pid_file;
	pid=daemon_pid_file_is_running();
	if (pid>=0) {
		daemon_log(LOG_ERR, "Daemon already running on PID file %u", pid);
		exit(EXIT_FAILURE);
	}
	if (daemon_retval_init() < 0) {
		daemon_log(LOG_ERR, "Failed to create pipe.");
		exit(EXIT_FAILURE);
	}
}


void ProxySQL_daemonize_wait_daemon() {
	int ret;
	/* Wait for 20 seconds for the return value passed from the daemon process */
	if ((ret = daemon_retval_wait(20)) < 0) {
		daemon_log(LOG_ERR, "Could not receive return value from daemon process: %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (ret) {
		daemon_log(LOG_ERR, "Daemon returned %i as return value.", ret);
	}
	exit(ret);
}


bool ProxySQL_daemonize_phase2() {
	int rc;
/*
	// we DO NOT close FDs anymore. See:
	// https://github.com/sysown/proxysql/issues/2628
	//
	// Close FDs
	if (daemon_close_all(-1) < 0) {
		daemon_log(LOG_ERR, "Failed to close all file descriptors: %s", strerror(errno));

		// Send the error condition to the parent process
		daemon_retval_send(1);
		return false;
	}
*/

	rc=chdir(GloVars.datadir);
	if (rc) {
		daemon_log(LOG_ERR, "Could not chdir into datadir: %s . Error: %s", GloVars.datadir, strerror(errno));
		exit(EXIT_FAILURE);
	}
	/* Create the PID file */
	if (daemon_pid_file_create() < 0) {
		daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
		daemon_retval_send(2);
		return false;
	}

	/* Send OK to parent process */
	daemon_retval_send(0);
	GloAdmin->flush_error_log();
	//daemon_log(LOG_INFO, "Starting ProxySQL\n");
	//daemon_log(LOG_INFO, "Sucessfully started");
	proxy_info("Starting ProxySQL\n");
	proxy_info("Successfully started\n");
	return true;
}


void call_execute_on_exit_failure() {
	if (GloVars.execute_on_exit_failure == NULL) {
		return;
	}
	proxy_error("Trying to call external script after exit failure: %s\n", GloVars.execute_on_exit_failure);
	pid_t cpid;
	cpid = fork();
	if (cpid == -1) {
		exit(EXIT_FAILURE);
	}
	if (cpid == 0) {
		int rc;
		rc = system(GloVars.execute_on_exit_failure);
		if (rc) {
			proxy_error("Execute on EXIT_FAILURE: Failed to run %s\n", GloVars.execute_on_exit_failure);
			perror("system()");
			exit(EXIT_FAILURE);
		} else {
			exit(EXIT_SUCCESS);
		}
	} else {
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize (&attr, 64*1024);
		pid_t *cpid_ptr=(pid_t *)malloc(sizeof(pid_t));
		*cpid_ptr=cpid;
		pthread_t thr;
		if (pthread_create(&thr, &attr, waitpid_thread, (void *)cpid_ptr) !=0 ) {
			perror("Thread creation");
			exit(EXIT_FAILURE);
		}
	}
}


bool ProxySQL_daemonize_phase3() {
	int rc;
	int status;
	//daemon_log(LOG_INFO, "Angel process started ProxySQL process %d\n", pid);
	parent_open_error_log();
	proxy_info("Angel process started ProxySQL process %d\n", pid);
	parent_close_error_log();
	rc=waitpid(pid, &status, 0);
	if (rc==-1) {
		parent_open_error_log();
		perror("waitpid");
		//proxy_error("[FATAL]: waitpid: %s\n", perror("waitpid"));
		exit(EXIT_FAILURE);
	}
	rc=WIFEXITED(status);
	if (rc) { // client exit()ed
		rc=WEXITSTATUS(status);
		if (rc==0) {
			//daemon_log(LOG_INFO, "Shutdown angel process\n");
			parent_open_error_log();
			proxy_info("Shutdown angel process\n");
			exit(EXIT_SUCCESS);
		} else {
			//daemon_log(LOG_INFO, "ProxySQL exited with code %d . Restarting!\n", rc);
			parent_open_error_log();
			proxy_error("ProxySQL exited with code %d . Restarting!\n", rc);
			call_execute_on_exit_failure();
			parent_close_error_log();
			return false;
		}
	} else {
		//daemon_log(LOG_INFO, "ProxySQL crashed. Restarting!\n");
		parent_open_error_log();
		proxy_error("ProxySQL crashed. Restarting!\n");
		proxy_info("ProxySQL version %s\n", PROXYSQL_VERSION);
		if (binary_sha1) {
			proxy_info("ProxySQL SHA1 checksum: %s\n", binary_sha1);
		}
		call_execute_on_exit_failure();
		parent_close_error_log();
		return false;
	}
	return true;
}

void my_terminate(void) {
	proxy_error("ProxySQL crashed due to exception\n");
	print_backtrace();
}

namespace {
	static const bool SET_TERMINATE = std::set_terminate(my_terminate);
}

// 关闭所有持有指定文件名的文件描述符
void close_fds_for_file(const char *filename) {
    DIR *proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        perror("opendir /proc");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(proc_dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue; // 跳过非进程目录
        }

        pid_t pid = atoi(entry->d_name);
        char fd_path[100];
        snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd", pid);

        DIR *fd_dir = opendir(fd_path);
        if (fd_dir == NULL) {
            continue;
        }

        struct dirent *fd_entry;
        while ((fd_entry = readdir(fd_dir)) != NULL) {
            if (fd_entry->d_name[0] == '.') {
                continue;
            }

            char link_path[200];
            snprintf(link_path, sizeof(link_path), "%s/%s", fd_path, fd_entry->d_name);
            char target[200];
            ssize_t len = readlink(link_path, target, sizeof(target) - 1);
            if (len != -1) {
                target[len] = '\0';
                if (strstr(target, filename) != NULL) {
                    int fd = atoi(fd_entry->d_name);
					printf("Find fd = %d\n", fd);
                    if (close(fd) == -1) {
                        perror("close");
                    }
                }
            }
        }
        closedir(fd_dir);
    }
    closedir(proc_dir);
}

// 批量清理多个文件
void cleanup_multiple_files(const char *files[], int num_files) {
    for (int i = 0; i < num_files; i++) {
        printf("Cleaning file descriptors for %s...\n", files[i]);
        close_fds_for_file(files[i]);
    }
}

int clean_files() {
    const char *files[] = {
        "proxysql-cert.pem",
        "proxysql.db",
        "proxysql-ca.pem",
        "proxysql-key.pem",
        "proxysql_stats.db"
    };
    int num_files = sizeof(files) / sizeof(files[0]);

    cleanup_multiple_files(files, num_files);

    return 0;
}


int main(int argc, const char * argv[]) {

	clean_files();

	{
		MYSQL *my = mysql_init(NULL);
		mysql_close(my);
//		cpu_timer t;
		ProxySQL_Main_init();
#ifdef DEBUG
//		std::cerr << "Main init phase0 completed in ";
#endif
	}
	{
		cpu_timer t;
		ProxySQL_Main_process_global_variables(argc, argv);
		GloVars.global.start_time=monotonic_time(); // always initialize it
		srand(GloVars.global.start_time*thread_id());
		randID = rand();
#ifdef DEBUG
		std::cerr << "Main init global variables completed in ";
#endif
	}

	struct rlimit nlimit;
	{
		int rc = getrlimit(RLIMIT_NOFILE, &nlimit);
		if (rc == 0) {
			proxy_info("Current RLIMIT_NOFILE: %lu\n", nlimit.rlim_cur);
			if (nlimit.rlim_cur <= 1024) {
				proxy_error("Current RLIMIT_NOFILE is very low: %lu .  Tune RLIMIT_NOFILE correctly before running ProxySQL\n", nlimit.rlim_cur);
				if (nlimit.rlim_max > nlimit.rlim_cur) {
					if (nlimit.rlim_max >= 102400) {
						nlimit.rlim_cur = 102400;
					} else {
						nlimit.rlim_cur = nlimit.rlim_max;
					}
					proxy_warning("Automatically setting RLIMIT_NOFILE to %lu\n", nlimit.rlim_cur);
					rc = setrlimit(RLIMIT_NOFILE, &nlimit);
					if (rc) {
						proxy_error("Unable to increase RLIMIT_NOFILE: %s: \n", strerror(errno));
					}
				} else {
					proxy_error("Unable to increase RLIMIT_NOFILE because rlim_max is low: %lu\n", nlimit.rlim_max);
				}
			}
		} else {
			proxy_error("Call to getrlimit failed: %s\n", strerror(errno));
		}
	}

	{
		cpu_timer t;
		ProxySQL_Main_init_SSL_module();
#ifdef DEBUG
		std::cerr << "Main SSL init variables completed in ";
#endif
	}

	{
		cpu_timer t;
		int fd = -1;
		char buff[PATH_MAX+1];
		ssize_t len = -1;
#if defined(__FreeBSD__)
		len = readlink("/proc/curproc/file", buff, sizeof(buff)-1);
#else
		len = readlink("/proc/self/exe", buff, sizeof(buff)-1);
#endif
		if (len != -1) {
			buff[len] = '\0';
			fd = open(buff, O_RDONLY);
		}
		if(fd >= 0) {
			struct stat statbuf;
			if(fstat(fd, &statbuf) == 0) {
				unsigned char *fb = (unsigned char *)mmap(0, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
				if (fb != MAP_FAILED) {
					unsigned char temp[SHA_DIGEST_LENGTH];
					SHA1(fb, statbuf.st_size, temp);
					binary_sha1 = (char *)malloc(SHA_DIGEST_LENGTH*2+1);
					memset(binary_sha1, 0, SHA_DIGEST_LENGTH*2+1);
					char buf[SHA_DIGEST_LENGTH*2 + 1];
					for (int i=0; i < SHA_DIGEST_LENGTH; i++) {
						sprintf((char*)&(buf[i*2]), "%02x", temp[i]);
					}
					memcpy(binary_sha1, buf, SHA_DIGEST_LENGTH*2);
					munmap(fb,statbuf.st_size);
				} else {
					proxy_error("Unable to mmap %s: %s\n", buff, strerror(errno));
				}
			} else {
				proxy_error("Unable to fstat %s: %s\n", buff, strerror(errno));
			}
		} else {
			proxy_error("Unable to open %s: %s\n", argv[0], strerror(errno));
		}
#ifdef DEBUG
		std::cerr << "SHA1 generated in ";
#endif
	}
	if (GloVars.global.foreground==false) {
		{
			cpu_timer t;
			ProxySQL_daemonize_phase1((char *)argv[0]);
#ifdef DEBUG
			std::cerr << "Main daemonize phase1 completed in ";
#endif
		}
	/* Do the fork */
		if ((pid = daemon_fork()) < 0) {
			/* Exit on error */
			daemon_retval_done();
			exit(EXIT_FAILURE);

		} else if (pid) { /* The parent */

			ProxySQL_daemonize_wait_daemon();

		} else { /* The daemon */

			cpu_timer t;
			GloVars.global.start_time=monotonic_time();
			GloVars.install_signal_handler();
			if (ProxySQL_daemonize_phase2()==false) {
				goto finish;
			}

#ifdef DEBUG
			std::cerr << "Main daemonize phase1 completed in ";
#endif
		}

	laststart=0;
	if (glovars.proxy_restart_on_error) {
gotofork:
		if (laststart) {
			int currenttime=time(NULL);
			if (currenttime == laststart) { /// we do not want to restart multiple times in the same second
				// if restart is too frequent, something really bad is going on
				//daemon_log(LOG_INFO, "Angel process is waiting %d seconds before starting a new ProxySQL process\n", glovars.proxy_restart_delay);
				parent_open_error_log();
				proxy_info("Angel process is waiting %d seconds before starting a new ProxySQL process\n", glovars.proxy_restart_delay);
				parent_close_error_log();
				sleep(glovars.proxy_restart_delay);
			}
		}
		laststart=time(NULL);
		pid = fork();
		if (pid < 0) {
			//daemon_log(LOG_INFO, "[FATAL]: Error in fork()\n");
			parent_open_error_log();
			proxy_error("[FATAL]: Error in fork()\n");
			exit(EXIT_FAILURE);
		}

		if (pid) { /* The parent */

			parent_close_error_log();
			if (ProxySQL_daemonize_phase3()==false) {
				goto gotofork;
			}

		} else { /* The daemon */

			// we open the files also on the child process
			// this is required if the child process was created after a crash
			parent_open_error_log();
			GloVars.global.start_time=monotonic_time();
			GloVars.install_signal_handler();
		}
	}



	} else {
		GloAdmin->flush_error_log();
		GloVars.install_signal_handler();
	}

__start_label:

	{
		cpu_timer t;
		ProxySQL_Main_init_phase2___not_started();
#ifdef DEBUG
		std::cerr << "Main init phase2 completed in ";
#endif
	}
	if (glovars.shutdown) {
		goto __shutdown;
	}

	{
		cpu_timer t;
		ProxySQL_Main_init_phase3___start_all();
#ifdef DEBUG
		std::cerr << "Main init phase3 completed in ";
#endif
	}
#ifdef DEBUG
		std::cerr << "WARNING: this is a DEBUG release and can be slow or perform poorly. Do not use it in production" << std::endl;
#endif
	proxy_info("For information about products and services visit: https://proxysql.com/\n");
	proxy_info("For online documentation visit: https://proxysql.com/documentation/\n");
	proxy_info("For support visit: https://proxysql.com/services/support/\n");
	proxy_info("For consultancy visit: https://proxysql.com/services/consulting/\n");

	{
		unsigned int missed_heartbeats = 0;
		unsigned long long previous_time = monotonic_time();
		unsigned int inner_loops = 0;
		unsigned long long time_next_version_check = 0;
		while (glovars.shutdown==0) {
			usleep(200000);
			if (disable_watchdog) {
				continue;
			}
			unsigned long long curtime = monotonic_time();
			if (GloVars.global.version_check) {
				if (curtime > time_next_version_check) {
					pthread_attr_t attr;
					pthread_attr_init(&attr);
					pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
					pthread_t thr;
					if (pthread_create(&thr, &attr, main_check_latest_version_thread, NULL) !=0 ) {
						perror("Thread creation");
						exit(EXIT_FAILURE);
					}
					if (time_next_version_check == 0)
						time_next_version_check = curtime;
					unsigned long long inter = 24*3600*1000;
					inter *= 1000;
					time_next_version_check += inter;
				}
			}
			inner_loops++;
			if (curtime >= inner_loops*300000 + previous_time ) {
				// if this happens, it means that this very simple loop is blocked
				// probably we are running under gdb
				previous_time = curtime;
				inner_loops = 0;
				continue;
			}
			if (GloMTH) {
				unsigned long long atomic_curtime = 0;
				unsigned long long poll_timeout = (unsigned int)GloMTH->variables.poll_timeout;
				unsigned int threads_missing_heartbeat = 0;
				poll_timeout += 1000; // add 1 second (rounding up)
				poll_timeout *= 1000; // convert to us
				if (curtime < previous_time + poll_timeout) {
					continue;
				}
				previous_time = curtime;
				inner_loops = 0;
				unsigned int i;
				if (GloMTH->mysql_threads) {
					for (i=0; i<GloMTH->num_threads; i++) {
						if (GloMTH->mysql_threads[i].worker) {
							atomic_curtime = GloMTH->mysql_threads[i].worker->atomic_curtime;
							if (curtime > atomic_curtime + poll_timeout) {
								threads_missing_heartbeat++;
							}
						}
					}
				}
#ifdef IDLE_THREADS
				if (GloVars.global.idle_threads) {
					if (GloMTH->mysql_threads) {
						for (i=0; i<GloMTH->num_threads; i++) {
							if (GloMTH->mysql_threads_idles[i].worker) {
								atomic_curtime = GloMTH->mysql_threads_idles[i].worker->atomic_curtime;
								if (curtime > atomic_curtime + poll_timeout) {
									threads_missing_heartbeat++;
								}
							}
						}
					}
				}
#endif
				if (threads_missing_heartbeat) {
					proxy_error("Watchdog: %u threads missed a heartbeat\n", threads_missing_heartbeat);
					missed_heartbeats++;
					if (missed_heartbeats >= (unsigned int)GloVars.restart_on_missing_heartbeats) {
#ifdef RUNNING_ON_VALGRIND
						proxy_error("Watchdog: reached %u missed heartbeats. Not aborting because running under Valgrind\n", missed_heartbeats);
#else
						if (GloVars.restart_on_missing_heartbeats) {
							proxy_error("Watchdog: reached %u missed heartbeats. Aborting!\n", missed_heartbeats);
							proxy_error("Watchdog: see details at https://github.com/sysown/proxysql/wiki/Watchdog\n");
							assert(0);
						}
#endif
					}
				} else {
					missed_heartbeats = 0;
				}
			}
		}
	}

__shutdown:

	proxy_info("Starting shutdown...\n");

	// First shutdown step is to unload plugins
	UnloadPlugins();

	ProxySQL_Main_init_phase4___shutdown();

	proxy_info("Shutdown completed!\n");

	if (glovars.reload) {
		if (glovars.reload==2) {
			GloVars.global.nostart=true;
		}
		glovars.reload=0;
		glovars.shutdown=0;
		goto __start_label;
	}

finish:
	//daemon_log(LOG_INFO, "Exiting...");
	proxy_info("Exiting...\n");
	daemon_retval_send(255);
	daemon_signal_done();
	daemon_pid_file_remove();

//	l_mem_destroy(__thr_sfp);

#ifdef RUNNING_ON_VALGRIND
	if (RUNNING_ON_VALGRIND==0) {
		if (__web_interface) {
			dlclose(__web_interface);
		}
		if (__mysql_ldap_auth) {
			dlclose(__mysql_ldap_auth);
		}
	}
#endif
	return 0;
}
