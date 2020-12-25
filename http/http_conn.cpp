#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

//将用户名和密码存入user中
void http_conn::initmysql_result(connection_pool *connPool)
{
	MYSQL *mysql = nullptr;
	connectionRAII mysqlcon(&mysql, connPool);

	if(mysql_query(mysql, "SELECT username,passwd FROM user"))
	{
		LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
	}
	
	MYSQL_RES *result = mysql_store_result(mysql);

	int num_fields = mysql_num_fields(result);

	MYSQL_FIELD *fields = mysql_fetch_fields(result);

	while(MYSQL_ROW row = mysql_fetch_row(result))
	{
		string temp1(row[0]);
		string temp2(row[1]);
		users[temp1] = temp2;
	}
}

int setnonblocking(int fd)
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
	epoll_event event;
	event.data.fd = fd;

	if (1 == TRIGMode)
		event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	else
		event.events = EPOLLIN | EPOLLRDHUP;

	if (one_shot)
		event.events |= EPOLLONESHOT;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
	epoll_event event;
	event.data.fd = fd;

	if (1 == TRIGMode)
		event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
	else
		event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

	epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
	if(real_close && (m_sockfd != -1))
	{
		printf("close %d\n", m_sockfd);
		removefd(m_epollfd, m_sockfd);
		m_sockfd = -1;
		--m_user_count;
	}
}

void http_conn::init(int sockfd,const sockaddr_in &addr, char *root, int TRIGMode,
					 int close_log, string user, string passwd, string sqlname)
{
	m_sockfd = sockfd;
	m_address = addr;

	addfd(m_epollfd,sockfd,true, m_TRIGMode);
	++m_user_count;

	doc_root = root;
	m_TRIGMode = TRIGMode;
	m_close_log = close_log;

	strcpy(sql_user, user.c_str());
	strcpy(sql_passwd, passwd.c_str());
	strcpy(sql_name, sqlname.c_str());

	init();
}

void http_conn::init()
{
	mysql = NULL;
	bytes_to_send = 0;
	bytes_have_send = 0;
	m_check_state = CHECK_STATE_REQUESTLINE;
	m_linger = false;
	m_method = GET;
	m_url = 0;
	m_version = 0;
	m_content_length = 0;
	m_host = 0;
	m_start_line = 0;
	m_checked_idx = 0;
	m_read_idx = 0;
	m_write_idx = 0;
	cgi = 0;
	m_state = 0;
	timer_flag = 0;
	improv = 0;

	memset(m_read_buf, '\0', READ_BUFFER_SIZE);
	memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
	memset(m_real_file, '\0', FILENAME_LEN);
}

http_conn::LINE_STATUS http_conn::parse_line()
{
	char temp;
	for(; m_checked_idx < m_read_idx; ++m_checked_idx)
	{
		temp = m_read_buf[m_checked_idx];
		if(temp == '\r')
		{
			if((m_checked_idx + 1) == m_read_idx)
				return LINE_OPEN;
			else if (m_read_buf[m_checked_idx+1] == '\n')
			{
				m_read_buf[m_checked_idx++] = '\0';
				m_read_buf[m_checked_idx++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
		else if (temp == '\n')
		{
			if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
			{
				m_read_buf[m_checked_idx - 1] = '\0';
				m_read_buf[m_checked_idx++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
		
	}
	return LINE_OPEN;
}

bool http_conn::read_once()
{
	if(m_read_idx >= READ_BUFFER_SIZE)
	{
		return false;
	}
	int bytes_read = 0;

	if(m_TRIGMode == 0)
	{
		bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
		m_read_idx += bytes_read;

		if(bytes_read <= 0)
		{
			return false;
		}

		return true;
	}
	else
	{
		while(true)
		{
			bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
			if (bytes_read == -1)
			{
				if(errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				return false;
			}
			else if(bytes_read == 0)
			{
				return false;
			}
			m_read_idx += bytes_read;
		}
		return true;
	}
	
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
	m_url = strpbrk(text, " \t");
	if(!m_url)
		return BAD_REQUEST;

	*m_url++ = '\0';
	char *method = text;
	if(strcasecmp(method, "GET") == 0)
		m_method = GET;
	else if(strcasecmp(method, "POST") == 0)
	{
		m_method = POST;
		cgi = 1;
	}
	else {
		return BAD_REQUEST;
	}

	m_url += strspn(m_url, " \t");
	m_version = strpbrk(m_url, " \t");

	if(!m_version)
		return BAD_REQUEST;

	*m_version++ = '\0';
	m_version += strspn(m_version, " \t");
	if (strcasecmp(m_version, "HTTP/1.1") != 0)
		return BAD_REQUEST;
	if(strncasecmp(m_url, "http://", 7) == 0)
	{
		m_url += 7;
		m_url = strchr(m_url,'/');
	}

	if(strncasecmp(m_url, "http://", 8) == 0)
	{
		m_url += 8;
		m_url = strchr(m_url,'/');
	}	

	if(!m_url || m_url[0] != '/')
		return BAD_REQUEST;

	if (strlen(m_url) == 1)
		strcat(m_url, "judge.html");
	
	m_check_state = CHECK_STATE_HEADER;

	return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
	if(text[0] == '\0')
	{
		if(m_content_length != 0)
		{
			m_check_state = CHECK_STATE_CONTENT;
			return NO_REQUEST;
		}
		return GET_REQUEST;
	}else if(strncasecmp(text, "Connection:", 11) == 0)
	{
		text += 11;
		text += strspn(text, " \t");
		if (strcasecmp(text, "keep-alive") == 0)
		{
			m_linger = true;
		}
	}else if(strncasecmp(text, "Content-length:", 15) == 0)
	{
		text += 15;
		text += strspn(text, " \t");
		m_content_length = atol(text);
	}else if(strncasecmp(text, "Host:", 5) == 0)
	{
		text += 5;
		text += strspn(text, " \t");
		m_host = text;
	}else
	{
		LOG_INFO("oop!unknow header: %s", text);
	}

	return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
	if (m_read_idx >= (m_content_length + m_checked_idx))
	{
		text[m_content_length] = '\0';
		m_string = text;
		return GET_REQUEST;
	}
	return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
	LINE_STATUS line_status = LINE_OK;
	HTTP_CODE ret = NO_REQUEST;
	char *text = 0;

	while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
	{
		text = get_line();
		m_start_line = m_checked_idx;
		LOG_INFO("%s", text);

		switch (m_check_state)
		{
		case CHECK_STATE_REQUESTLINE:
		{
			ret = parse_request_line(text);
			if (ret == BAD_REQUEST)
				return BAD_REQUEST;
			break;
		}
		case CHECK_STATE_HEADER:
		{
			ret = parse_headers(text);
			if (ret == BAD_REQUEST)
				return BAD_REQUEST;
			else if (ret == GET_REQUEST)
			{
				return do_request();
			}
			break;
		}
		case CHECK_STATE_CONTENT:
		{
			ret = parse_content(text);
			if (ret == GET_REQUEST)
				return do_request();
			line_status = LINE_OPEN;
			break;
		}
		default:
			return INTERNAL_ERROR;
		}
	}

	return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);

	const char *p = strrchr(m_url, '/');

	if( cgi == 1 && (*(p+1) == '2' || *(p+1) == '3'))
	{
		char flag = m_url[1];
		
		char *m_url_real = (char*)malloc(sizeof(char) * 200);
		strcpy(m_url_real, "/");
		strcat(m_url_real, m_url+2);
		strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len -1);
		free(m_url_real);

		char name[100], password[100];
		int i;
		for( i = 5; m_string[i] != '&' ; ++i)
			name[i-5] = m_string[i];
		name[i-5] = '\0';

		int j = 0;
		for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
			password[j] = m_string[i];
		password[j] = '\0';

		if(*(p+1) == '3')
		{
			char *sql_insert = (char *)malloc(sizeof(char) * 200);
			strcpy(sql_insert,"INSERT INTO user(username, passwd) VALUES(");
			strcat(sql_insert, "'");
			strcat(sql_insert, name);
			strcat(sql_insert, "', '");
			strcat(sql_insert, password);
			strcat(sql_insert, "')");

			if(users.find(name) == users.end())
			{
				m_lock.lock();
				int res = mysql_query(mysql, sql_insert);
				users.insert(pair<string, string>(name, password));
				m_lock.unlock();

				if(!res)
					strcpy(m_url, "/log.html");
				else
					strcpy(m_url, "/registerError.html");
			}
			else
				strcpy(m_url, "/registerError.html");
		}else if(*(p + 1) == '2')
		{
			if (users.find(name) != users.end() && users[name] == password)
				strcpy(m_url, "/welcome.html");
			else
				strcpy(m_url, "/logError.html");
		}
	}

	
}