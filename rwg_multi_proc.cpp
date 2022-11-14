#include "functions_multi_proc.h"
#include "rwg_multi_proc.h"

std::vector<my_cmd> C; // after read one line of commands, stores them here
std::map< size_t, args > args_of_cmd; // pid, args
my_cmd tmp; // used in clear_tmp() and process_pipe_info()
std::vector<pnt> pipe_num_to; // pipe_num, counter   

client_pid me;
int FIFO_open[2][30];

int main(int argc, char **argv)
{
    setenv("PATH", "bin:.", 1);
    signal(SIGCHLD, sig_chld);
    signal(SIGINT, sig_term);

    me = cp[atoi(argv[1])];
    for (int i = 0; i < 2; i++)
    {
        shm_key[i] = (key_t)(1234+i);
        if ((shm_id[i] = shmget(shm_key[i], SHM_SIZE*OPEN_MAX, PERMS|IPC_CREAT)) < 0) err_sys("shmget fail");
    } 

    while (true)
    {
        if (std::cin.eof()) break;

        printf("%% "); fflush(stdout);

        char buf[MY_LINE_MAX];
        int n = read(STDIN_FILENO, buf, MY_LINE_MAX);

		if (n < 0) {
			err_sys("failed to read\n");
			return -1;
		}
		else if (n <= 1) continue; // blank line

        int status;
        if ((status = execute_line(buf)) == -1) break;
        else if (status == -2) return 0;
    }

    wait_all_children();
    sig_term(0);

    return 0;
}

int execute_line(char *line)
{
    parse_line(line);

    int status = 1;
    for (int i = 0; i < C.size(); i++)
    {
        if (i % 50 == 0 && i != 0) conditional_wait();
        if ((status = execute_command(C[i])) < 0) return -1; // exit        
    }
    if (status == 0) update_pipe_num_to();
    conditional_wait();
    C.clear();
    
    return 0;
}

int execute_command(my_cmd &command)
{
    if (command.argv[0] == "exit") {
        C.clear();
        return -1;
    }
    if (command.argv[0] == "setenv") 
    {
        my_setenv(command);
        return 0;
    }
    if (command.argv[0] == "printenv") 
    {
        my_printenv(command);
        return 0;
    }
    if (command.argv[0] == "who")
    {
        print_all_user();
        return 0;
    }
    if (command.argv[0] == "tell")
    {
        if (command.argv.size() < 3) 
        {
            char err[] = "Usage: tell <to whom> <msg>\n";
            Writen(STDOUT_FILENO, err, strlen(err));
            return 0;
        }
        // std::string msg = "";
        // for (int i = 2; i < command.argv.size(); i++) msg += command.argv[i];
        tell_msg(atoi(command.argv[1].data()), command.argv[2]);
        return 0;
    }
    if (command.argv[0] == "yell")
    {
        if (command.argv.size() < 2) 
        {
            char err[] = "Usage: yel <msg>\n";
            Writen(STDOUT_FILENO, err, strlen(err));
            return 0;
        }
        yell_msg(command.argv[1]);
        return 0;
    }
    if (command.argv[0] == "name")
    {
        if (command.argv.size() < 2) 
        {
            char err[] = "Usage: name <name>\n";
            Writen(STDOUT_FILENO, err, strlen(err));
            return 0;
        }
        change_name(command.argv[1]);
        return 0;
    }

    args cmd;
    cmd.argc = command.argv.size();
    cmd.argv = (char**) malloc(sizeof(char*) * (cmd.argc+1));
    cmd.number_pipe = command.number_pipe;

    bool need_data, need_pipe = (command.pipe_to > 0);
    bool u_pipe_to = false, u_pipe_from = false;
    int u_to, u_from;
    size_t pid;
    int p_num[2], data_pipe[2], u_pipe[2];
    std::vector<int> data_list;

    // record pipe descripter for the command behind to read
    if (need_pipe) 
    {
        pipe(p_num);
        cmd.p_num = p_num[0];
    }

    // received data from user pipe
    int from = command.user_pipe_from;
    if (from >= 0)
    {
        u_pipe_from = true;

        if (check_user_pipe_from(from, u_from) < 0) u_from = open("/dev/null", O_RDONLY);
        else 
        {
            std::string cmd_line = command.user_pipe_command;
            cmd_line.erase(cmd_line.end()-1);
            
            char msg[MY_LINE_MAX +50];
            memset(msg, '\0', MY_LINE_MAX+50);

            client_pid c(from);
            read_user_info(c);

            sprintf(msg, "*** %s (#%d) just received from %s (#%d) by '%s' ***\n", me.name, me.id+1, c.name, from+1, cmd_line.data());
            broadcast(msg);

            cmd.from = from;
        }

        
    }

    // user pipe to other user
    
    int to = command.user_pipe_to;
    if (to >= 0)
    {
        u_pipe_to = true;

        if (check_user_pipe_to(to, u_to) < 0) u_to = open("/dev/null", O_WRONLY);
        else 
        {
            std::string cmd_line = command.user_pipe_command;
            cmd_line.erase(cmd_line.end()-1);

            char msg[MY_LINE_MAX +50];
            memset(msg, '\0', MY_LINE_MAX+50); 

            client_pid c(from);
            read_user_info(c);

            sprintf(msg, "*** %s (#%d) just piped '%s' to %s (#%d) ***\n", me.name, me.id+1, cmd_line.data(), c.name, c.id+1);
            broadcast(msg);

            cmd.to = to;
        }
    }

    check_need_data(need_data, data_pipe, data_list);
  
    // fork
    pid = fork();
    if (pid < 0) 
    {
        char *err;
        sprintf(err, "failed to fork\n");
        Writen(STDERR_FILENO, err, strlen(err));
        return 0;
    }

    // parent do
    if (pid > 0)
    {
        if (need_pipe) close(p_num[1]);
        if (need_data) close(data_pipe[0]);

        if (u_pipe_to) close(u_to);
        if (u_pipe_from) close(u_from);

        // record memory allocate address
        args_of_cmd.insert(std::pair<size_t, args> (pid, cmd));
        
        // update when new line
        if (command.number_pipe) update_pipe_num_to();
        
        // store read id of pipe
        pnt tmp;
        if (command.number_pipe) {
            tmp.set(p_num[0], command.pipe_to-1);
            pipe_num_to.push_back(tmp);
        }
        else if (need_pipe) 
        {
            tmp.set(p_num[0], 0);
            pipe_num_to.push_back(tmp);
        }

        int status;
        // process data from multiple pipe
        if (need_data && data_list.size() != 0) 
            if ((status = handle_data_from_multiple_pipe(data_pipe, data_list)) < 0) return status;
        
        // update when new line
        if (command.number_pipe) return 1;
        else return 0;
    }
    //child do
    else {

        // received data from other process
        if (need_data) 
        {
            // parent will handle multiple pipes of data
            if (data_list.size() != 0) 
            {
                close(data_pipe[1]);
                for(auto id: data_list) close(id);
            }

            dup2(data_pipe[0], STDIN_FILENO);
        }
        else if (u_pipe_from) dup2(u_to, STDIN_FILENO);

        // pipe stderr
        if (command.err) dup2(p_num[1], STDERR_FILENO);

        // pipe data to other process
        if (need_pipe) {
            close(p_num[0]);
            dup2(p_num[1], STDOUT_FILENO);
        }
        else if (u_pipe_to) 
        {
            close(u_pipe[0]);
            dup2(u_pipe[1], STDOUT_FILENO);
        }

        // store data to a file
        if (command.store_addr.size() > 0) {
            if (set_output_to_file(command) < 0)
            {
                free(cmd.argv);
                return 0;
            }
        }

        // get the arguments ready
        for (int j = 0; j < cmd.argc; j++){
            cmd.argv[j] = (char*) malloc(sizeof(char) * command.argv[j].size()+1);
            strcpy(cmd.argv[j], command.argv[j].c_str());
        }
        cmd.argv[cmd.argc] = new char;
        cmd.argv[cmd.argc] = NULL;

        // exec!!!!
        if (execvp(cmd.argv[0], cmd.argv) < 0) {
            char err[1024];
            sprintf(err, "Unknown command: [%s].\n", cmd.argv[0]);
            Writen(STDERR_FILENO, err, strlen(err));

            // close pipe
            if (need_data) close(data_pipe[0]);
            if (need_pipe) close(p_num[1]);

            return -2;
        }
    }
    return 0;
}

void parse_line(char *line)
{
    // user pipe
    std::string s_line(line);
    
	bool storage_flg = false;
	const char *new_args = " \n\r";
	char *command = strtok(line, new_args);
	while(command != NULL){
		std::string s(command);

		// store the path
		if (storage_flg) {
			tmp.store_addr = s;
			storage_flg = false;
			C.push_back(tmp);
			clear_tmp();
			
			command = strtok(NULL, new_args);
			continue;
		}

        // parse msg
        if (s == "tell")
        {
            tmp.argv.push_back(s);

            char *to = strtok(NULL, " ");
            std::string t(to);
            tmp.argv.push_back(t);

            char *msg = strtok(NULL, "\n\r\0");
            std::string m(msg);
            tmp.argv.push_back(m);

            C.push_back(tmp);
            clear_tmp();
            return;
        }
        if (s == "yell")
        {
            tmp.argv.push_back(s);

            char *msg = strtok(NULL, "\n\r\0");
            std::string m(msg);
            tmp.argv.push_back(m);

            C.push_back(tmp);
            clear_tmp();
            return;
        }

		if (s == "|")
		{
			tmp.pipe_to = 1;
		
			// add commands
			C.push_back(tmp);
			clear_tmp();
		}
		else if (command[0] == '|' || command[0] == '!') 
		{
			process_pipe_info(command);
			
			// add commands
			C.push_back(tmp);
			clear_tmp();
		}
        else if (s[0] == '<')
        {
            s.erase(0, 1);
            tmp.user_pipe_from = atoi(s.data())-1;
            tmp.user_pipe_command = s_line;
        }
		else if (s[0] == '>') 
        {
            if (s.size() == 1) storage_flg = true;
            else 
            {
                s.erase(0, 1);
                tmp.user_pipe_to = atoi(s.data())-1;
                tmp.user_pipe_command = s_line;
            }
        }
		else tmp.argv.push_back(s);

		command = strtok(NULL, new_args);
	}
	if (tmp.argv.size() > 0) 
    {
        C.push_back(tmp);
    	clear_tmp();
    }
}

int check_user_pipe_from(int from, int &u_from)
{
    client_pid f(from);
    read_user_info(f);

    char FIFO_name[10];
    sprintf(FIFO_name, "%d_%d", from, me.id);

    /*check if user with id 'from' exist*/
    if (f.connfd < 0) 
    {
        char msg[MY_LINE_MAX +50];
        memset(msg, '\0', MY_LINE_MAX+50);  
        sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", from+1);
        Writen(STDOUT_FILENO, msg, strlen(msg));
        
        if (FIFO_open[0][from] > 0) FIFO_open[0][from] = -1;
        if (FIFO_open[1][from] > 0) FIFO_open[1][from] = -1;

        return -1;
    }

    /*check if user pipe 'from->me' exist, maybe store the information in share memory*/
    if (FIFO_open[0][from] = open(FIFO_name, O_RDONLY) < 0)
    {
        if (errno == EACCES)
        {
            char msg[MY_LINE_MAX +50];
            memset(msg, '\0', MY_LINE_MAX+50);
            sprintf(msg, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", from+1, me.id+1);
            Writen(STDOUT_FILENO, msg, strlen(msg));
            return -1;
        }
        char err[MSG_MAX];
        sprintf(err, "can't open read fifo: %s\n", FIFO_name);
        write(STDOUT_FILENO, err, strlen(err));
        return -1;
    }
    // if (FIFO_open[0][from] < 0)
    // {
    //     char msg[MY_LINE_MAX +50];
    //     memset(msg, '\0', MY_LINE_MAX+50);
    //     sprintf(msg, "*** Error: the pipe #%d->#%d does not exist yet. ***\n", from+1, me.id+1);
    //     Writen(STDOUT_FILENO, msg, strlen(msg));

    //     return -1;
    // }    

    return 0;
}

int check_user_pipe_to(int to, int &u_to)
{
    /*check if user with id 'to' exist*/
    client_pid t(to);
    read_user_info(t);

    char FIFO_name[10];
    sprintf(FIFO_name, "%d_%d", me.id, to);
    if (t.connfd < 0)
    {
        char msg[MY_LINE_MAX +50];
        memset(msg, '\0', MY_LINE_MAX+50); 
        sprintf(msg, "*** Error: user #%d does not exist yet. ***\n", to+1);
        Writen(STDOUT_FILENO, msg, strlen(msg));
        return -1;

        if (FIFO_open[0][t.id] > 0) FIFO_open[0][t.id] = -1;
        if (FIFO_open[1][t.id] > 0) FIFO_open[1][t.id] = -1;
    }

    // if (FIFO_open[1][t.id] > 0)
    // {
    //     char msg[MY_LINE_MAX +50];
    //     memset(msg, '\0', MY_LINE_MAX+50); 
    //     sprintf(msg, "*** Error: the pipe #%d->#%d already exists. ***\n", me.id+1, to+1);
    //     Writen(STDOUT_FILENO, msg, strlen(msg));
    //     return -1;
    // }

    /* open FIFO */
    // if ( (mknod(FIFO_name, S_IFIFO, 0) < 0) && (errno != EEXIST) )
    // {
    //     char err[MSG_MAX];
    //     sprintf(err, "can't create fifo: %s\n", FIFO_name);
    //     write(STDOUT_FILENO, err, strlen(err));
    //     return -1;
    // }                
    
    if (mknod(FIFO_name, S_IFIFO, 0) < 0)
    {
        if (errno == EEXIST) 
        {
            char msg[MY_LINE_MAX +50];
            memset(msg, '\0', MY_LINE_MAX+50); 
            sprintf(msg, "*** Error: the pipe #%d->#%d already exists. ***\n", me.id+1, to+1);
            Writen(STDOUT_FILENO, msg, strlen(msg));
            return -1;
        }
        char err[MSG_MAX];
        sprintf(err, "can't create fifo: %s\n", FIFO_name);
        write(STDOUT_FILENO, err, strlen(err));
        return -1;
    }  

    if (u_to = open(FIFO_name, O_WRONLY) < 0)
    {
        char err[MSG_MAX];
        sprintf(err, "can't open write fifl: %s\n", FIFO_name);
        write(STDOUT_FILENO, err, strlen(err));
        return -1;
    }
    
    FIFO_open[1][to] = u_to;
    return 0;
}

void my_setenv(my_cmd &command)
{
	if (command.argv.size() < 3) {
		char err[] = "Usage: setenv [Variable] [Value].\n";
		write(STDOUT_FILENO, err, strlen(err));
        return;
	}
	setenv(command.argv[1].data(), command.argv[2].data(), 1);
}

void my_printenv(my_cmd &command)
{
	if (command.argv.size() < 2) {
		char err[] = "Usage: printenv [Variable].\n";
        write(STDOUT_FILENO, err, strlen(err));
		return;
	}
	char* env_info = getenv(command.argv[1].data());
	if (env_info != NULL) {
        char msg[MSG_MAX];
        memset(msg, '\0', MSG_MAX);
        sprintf(msg, "%s\n", env_info);
        write(STDOUT_FILENO, msg, strlen(msg));
    }
}

void print_all_user()
{
    char *shm_addr;
    char n[4];
    memcpy(n, shm_addr, 4);
    int num_user = atoi(n)-1;
    
    char snd[MSG_MAX], now[SHM_SIZE];
    memset(snd, '\0', MSG_MAX);
    
    char msg[MSG_MAX];

    memset(msg, '\0', MSG_MAX);
    sprintf(msg, "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n");
    Writen(STDOUT_FILENO, msg, strlen(msg));
    
    client_pid c;
    for (int i = 0; i < OPEN_MAX && num_user > 0; i++)
    {       
        memcpy(now, shm_addr + i*SHM_SIZE, SHM_SIZE+4);
        sscanf(now, "%d %d %s %d", &c.connfd, &c.pid, c.addr, &c.port); 
        if (c.connfd < 0) continue;

        num_user--;
        
        memset(msg, '\0', MSG_MAX);
        sprintf(msg, "%d\t%s\t%s:%d\t%s\n", i+1, c.name, c.addr, c.port, (c.id == me.id)?" <-me":"");
        Writen(STDOUT_FILENO, msg, strlen(msg));
    }
    
    if (shmdt(shm_addr) < 0) perror("shmdt fail");
}

void tell_msg(int to, std::string msg)
{
    char snd[MSG_MAX];
    memset(snd, '\0', MSG_MAX);

    to -= 1;

    client_pid t(to);
    if (t.connfd < 0)
    {
        sprintf(snd, "*** Error: user #%d does not exist yet. ***\n", to+1);
        Writen(STDOUT_FILENO, snd, strlen(snd));
        return;
    }

    /* get connfd of 'to' and send the message */
    sprintf(snd, "*** %s told you ***: %s\n", me.name, msg.data());
    Writen(t.connfd, snd, strlen(snd));
}

void yell_msg(std::string msg)
{
    char snd[MSG_MAX];
    memset(snd, '\0', MSG_MAX);
    
    sprintf(snd, "*** %s yelled ***: %s\n", me.name, msg.data());
    broadcast(snd);
}

void change_name(std::string new_name)
{
    char *shm_addr;
    char n[4];
    memcpy(n, shm_addr, 4);
    int num_user = atoi(n)-1;
    
    char snd[MSG_MAX], now[SHM_SIZE];
    memset(snd, '\0', MSG_MAX);

    client_pid c;
    for (int i = 0; i < OPEN_MAX && num_user > 0; i++)
    {       
        if (i == me.connfd) continue; 

        memcpy(now, shm_addr + i*SHM_SIZE, SHM_SIZE+4);
        sscanf(now, "%d %d %s %d", &c.connfd, &c.pid, c.addr, &c.port); 
        if (c.connfd < 0) continue;

        num_user--;
        
        if (strcmp(c.name, new_name.c_str()) == 0) 
        {
            sprintf(snd, "*** User '%s' already exists. ***\n", new_name.data());
            Writen(STDOUT_FILENO, snd, strlen(snd));
            return;
        }
    }
    
    if (shmdt(shm_addr) < 0) perror("shmdt fail");

    strcpy(me.name, new_name.data());

    sprintf(snd, "*** User from %s:%d is named '%s'. ***\n", me.addr, me.port, me.name);
    broadcast(snd);
}

int handle_data_from_multiple_pipe(int data_pipe[2], std::vector<int> data_list)
{

	// fork a child to process
	int pid = fork();
    
	if (pid < 0) {
		char err[] = "failed to fork\n";
		Writen(STDERR_FILENO, err, strlen(err));
		return -1;
	}

	if (pid == 0) {
		// read data from each pipe (FIFO)
		for (auto id: data_list) {
			char read_data[1024];
			int read_length;
			while ((read_length = Readn(id, read_data, 1024)) > 0) {
				Writen(data_pipe[1], read_data, read_length);
			}
			close(id);
		}
		close(data_pipe[1]);
		return -2;
	}else{
		close(data_pipe[1]);
		for(auto id: data_list) close(id);
		data_list.clear();
		
		// information for signal handler about how to handle this child
		args cmd;
		cmd.argv = NULL; cmd.argc = 0;
		args_of_cmd.insert(std::pair<size_t, args> (pid, cmd));

		return 0;
	}

	return 0;
}

int set_output_to_file(my_cmd &command)
{
	size_t file_id = open(command.store_addr.data(), O_WRONLY|O_CREAT|O_TRUNC, 00777);
	// clear the file content
	ftruncate(file_id, 0);
	lseek(file_id, 0, SEEK_SET);
	if (file_id < 0) {
		char err[1024];
		sprintf(err, "Failed to open file: %s\n", command.store_addr.data());
		Writen(STDERR_FILENO, err, strlen(err));
		return -1;
	}
	dup2(file_id, STDOUT_FILENO);
	return 0;
}

void check_need_data(bool &need, int (&data_pipe)[2], std::vector<int> &data_list) {
	// get all needed data for current command into datalist 
	// (datalist stores the needed read end of the pipe)
	data_list.clear();

	// counter == 0 => pipe the data to the command that execute next
	for (auto i = pipe_num_to.begin(); i < pipe_num_to.end(); ) {
    	if (i->remain <= 0) {
            data_list.push_back(i->pipe_num);
            pipe_num_to.erase(i);
        }
        else i++;
	}

    need = (data_list.size() != 0);
	if (!need) return;
	// if only one pipe of data needed
	// directly assign read p_id to data_pipe
	else if(data_list.size() == 1) {
		data_pipe[0] = data_list[0];
		data_list.clear();
		return;
	}
	// parent will tackle the data from multiple pipe
	else pipe(data_pipe);
}

void update_pipe_num_to() { 

	for (auto i = pipe_num_to.begin(); i < pipe_num_to.end(); ) {
        i -> remain--;
		if (i->remain < 0) {
            pipe_num_to.erase(i);
        }
        else 
            i++;
	}
}

void clear_tmp() {
	tmp.argv.clear();
	tmp.pipe_to = 0;
	tmp.store_addr = "";
	tmp.err = false;
	tmp.number_pipe = false;
    tmp.user_pipe_to = -1;
    tmp.user_pipe_from = -1;
    tmp.user_pipe_command = "";
}

void process_pipe_info(std::string s) {
	// only process number pipe
	tmp.number_pipe = true;
	if (s[0] == '!') tmp.err = true;
	
    s.erase(0, 1);
	tmp.pipe_to = atoi(s.c_str());
}

void wait_all_children() {
	while (!args_of_cmd.empty()) {
		sig_chld(SIGCHLD);			
	}
}

void conditional_wait() {
	while(!args_of_cmd.empty()) {
		sig_chld(SIGCHLD);

		// stop waiting if remaining are all commands with number pipe
		// the output are not immediately needed
		bool must_wait = false;
		for (auto s:args_of_cmd) {
			int p_num = s.second.p_num;
            bool find = false;
			for (auto p:pipe_num_to) {
                if (p.pipe_num == p_num)
                {
                    find = true;
                    break;
                }
			}
            if (!find)
            {
                must_wait = true;
                break;
            }
		}
		if (!must_wait) {
			break;
		}
	}
}

void sig_chld(int signo)
{
	int	pid, stat;

	while ( (pid = waitpid(-1, &stat, WNOHANG)) > 0){
		
		// free the memory!!
		char **cmd_argv = args_of_cmd[pid].argv;
        if (cmd_argv != NULL) free(cmd_argv);
		
        if (args_of_cmd[pid].from > 0)
        {
            char FIFO_name[10];
            sprintf(FIFO_name, "%d_%d", args_of_cmd[pid].from, me.id);
            
            if (unlink(FIFO_name) < 0)
            {
                char err[MSG_MAX];
                sprintf(err, "can't unlink\n");
                write(STDOUT_FILENO, err, strlen(err));
            }
        }

        if (args_of_cmd[pid].to > 0)
        {
            char FIFO_name[10];
            sprintf(FIFO_name, "%d_%d", me.id, args_of_cmd[pid].to);
            
            if (unlink(FIFO_name) < 0)
            {
                char err[MSG_MAX];
                sprintf(err, "can't unlink \n");
                write(STDOUT_FILENO, err, strlen(err));
            }
        }
        
        args_of_cmd.erase(pid);		
    }

	return;
}

void sig_term(int signo)
{
// char msg[1024];
// sprintf(msg, "received sig tem\n");
// write(STDOUT_FILENO, msg, strlen(msg));

    /* the other end finish reading */
    // if (signo == SIGUSR1)
    // {
    //     int to = signo -30;
    //     FIFO_open[1][to] = -1;
    //     return;
    // }
    // /* open FIFO read */
    // if (signo == SIGUSR2)
    // {
    //     char FIFO_name[10];
    //     sprintf(FIFO_name, "%d_%d", signo, me.id);
        
    //     if (FIFO_open[0][signo] = open(FIFO_name, O_RDONLY) < 0)
    //     {
    //         char err[MSG_MAX];
    //         sprintf(err, "can't open read fifo: %s\n", FIFO_name);
    //         write(STDOUT_FILENO, err, strlen(err));
    //         return;
    //     }
    //     return;
    // }
    /* disconnection, close by server */
    if (signo == SIGINT)
    {
        /* close user FIFO */
        for (auto arg:args_of_cmd)
        {
            kill(arg.first, 9);
        }

        while (args_of_cmd.size() > 0) sig_chld(0);
    }
    /* receive broadcast msg */
    else if (signo == SIGIO)
    {
        char *shm_addr = (char *)shmat(shm_id[1], 0, 0);
        if (shm_addr == NULL) err_sys("shmat fail");

        char msg_len[4];
        char msg[SHM_SIZE];
        memcpy(msg_len, shm_addr, 4);
        memcpy(msg, shm_addr+4, atoi(msg_len));

        write(me.connfd, msg, atoi(msg_len));

        /* deal with open/close FIFO */

        return;
    }

    exit(0);
}
// void err_sys(const char* x) { 
//     perror(x); 
//     exit(1); 
// }

ssize_t	writen(int connfd, const char *buf, size_t size) {
	size_t	nremain;
	ssize_t	nwritten;
	const char *buf_now;

	buf_now = buf;
	nremain = size;
	while (nremain > 0) {
		if ( (nwritten = write(connfd, buf_now, nremain)) <= 0) {
			if (nwritten < 0 && errno == EINTR) nwritten = 0; // the error is not from write()
			else return -1; // write error
		}

		nremain -= nwritten;
		buf_now += nwritten;
	}
	return(size);
}

void Writen(int connfd, char *buf, size_t size) {
	if (writen(connfd, buf, size) != size)
		err_sys("writen error");
}

ssize_t	readn(int connfd, char *buf, size_t size) {
	size_t nremain;
	ssize_t	nread;
	char *buf_now;

	buf_now = buf;
	nremain = size;
	while (nremain > 0) {
		if ( (nread = read(connfd, buf_now, nremain)) < 0) {
			if (errno == EINTR) nread = 0; // the error is not from read
			else return -1; // read error
		} 
		else if (nread == 0) break; // EOF

		nremain -= nread;
		buf_now += nread;
	}
	return(size - nremain);
}

ssize_t Readn(int connfd, char *buf, size_t size) {
	size_t	n;

	if ( (n = readn(connfd, buf, size)) < 0)
		err_sys("readn error\n");
	return(n);
}