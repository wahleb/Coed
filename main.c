#define _GNU_SOURCE
#include <limits.h>
#include <ctype.h>
#include <error.h>
#include <curses.h>
#include <stdlib.h>
#include <stddef.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#include <unistd.h>
#include <sys/types.h>

#include <string.h>

#include <stdio.h>


#define USERS 32

#define NAME_LEN 60

#define DELAY 1

#define NO_USER (-2)

#define LINE_LEN 40 //entirely arbitrary
#define LONG_LINE_LEN ((int) offsetof(struct Line,longline))

#define GROW_INC 500

#define b_get(num) (data->text[(num)])

#define LENGTH_OF(line,block) ((line) == (block) ? LINE_LEN : LONG_LINE_LEN)


struct Line {
	char str[LINE_LEN];

	union {
		struct {
			int prev;
			int next;
			int lineno;//not currently used, may be inaccurate
			int buffer;
//			int seq;//todo
			int longline;
		};
		struct {
			int buf_owner;
			int buf_used;
			int buf_pos;
			int next_buf;
		};
		int next_block;
	};
};


struct Data {
	pthread_rwlock_t lock;
	int line_at[USERS];
	//NOT line number, but the index for the first block of the line
	//-2 indicates no user, -1 is a user with no location (?)
	int char_at[USERS];
	int user_buf[USERS];
	int first_line;
	int first_free_block;
	int blocks_avail;
	struct Line text[];
};

struct Data* data = NULL;
int shm_fd = -1;
char* shm_name = NULL;
int user_no = -1;
int lines_from_top = 0;
int horizontal_scroll = 0;
int blocks_known = 0;


void free_block(int block);
void flush_line(int line);

void grow_shared_mem() {
	int old_bc = data->blocks_avail;
	int new_bc = old_bc + GROW_INC;

	int ret;
	ret = ftruncate(shm_fd,sizeof(struct Data) + sizeof(struct Line) * new_bc);
	if(ret)
		error_at_line(1,0,__FILE__,__LINE__,"ftruncate failed");

	data = mremap(data,sizeof(struct Data) + sizeof(struct Line) * old_bc,
			sizeof(struct Data) + sizeof(struct Line) * new_bc,MREMAP_MAYMOVE);

	if(data == MAP_FAILED)
		error_at_line(1,0,__FILE__,__LINE__,"mremap failed");

	int i;
	for(i=old_bc;i<new_bc;++i)
		free_block(i);
	blocks_known = data->blocks_avail = new_bc;
}

void recognize_growth() {//a different process has increased the size of the shared memory file, we need to remap it to accommodate the new size
	data = mremap(data,sizeof(struct Data) + sizeof(struct Line) * blocks_known,
			sizeof(struct Data) + sizeof(struct Line) * data->blocks_avail,MREMAP_MAYMOVE);
	if(data == MAP_FAILED)
		error_at_line(1,0,__FILE__,__LINE__,"mremap failed");
	blocks_known = data->blocks_avail;
}


//assumes we hold write lock
int alloc_block() {
	if(data->first_free_block < 0)
		grow_shared_mem();

	int block = data->first_free_block;
	data->first_free_block = b_get(block).next_block;
	return block;
}

//assumes we hold write lock
//breaks everything if called on an already free block
void free_block(int block) {
	b_get(block).next_block = data->first_free_block;
	data->first_free_block = block;
}

void read_lock() {
	int ret = pthread_rwlock_rdlock(&data->lock);
	if(ret)
		error(1,0,"got error %d when requesting read lock",ret);
	if(data->blocks_avail != blocks_known)
		recognize_growth();
}

void write_lock() {
	pthread_rwlock_wrlock(&data->lock);
	if(data->blocks_avail != blocks_known)
		recognize_growth();
}

void relock_write() {
	pthread_rwlock_unlock(&data->lock);
	pthread_rwlock_wrlock(&data->lock);
	if(data->blocks_avail != blocks_known)
		recognize_growth();
	//__sync_synchronize();
}

void init_data() {
	pthread_rwlockattr_t attr;
	if(pthread_rwlockattr_init(&attr) ||
			pthread_rwlockattr_setpshared(&attr,PTHREAD_PROCESS_SHARED) ||
			pthread_rwlockattr_setkind_np(&attr,PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP))//not sure what exactly the nonrecursive part means, the documentation doesn't say
		error_at_line(1,0,__FILE__,__LINE__,"todo: error text");

	if(pthread_rwlock_init(&data->lock,&attr))
		error_at_line(1,0,__FILE__,__LINE__,"todo: error text");

	pthread_rwlockattr_destroy(&attr);

	data->first_free_block = -1000;//more likely to segfault immediately when an error occurs
	data->blocks_avail = 0;
	grow_shared_mem();

	data->first_line = -1;

	int i;
	for(i=0;i<USERS;++i)
		data->line_at[i] = NO_USER;
}

int add_user() {
	int i;
	for(i=0;i<USERS;++i) {
#if 0
		int val;
		asm("lock cmpxchg %2,%1;"
				:"=a"(val),"+m"(data->line_at[i])
				:"r"(-1),"0"(NO_USER)
		   );
		if(val == NO_USER)
			return i;
#else 
		if(__sync_bool_compare_and_swap(data->line_at +i,NO_USER,-1)){
			data->user_buf[i] = -1;
			return i;
		}
#endif
	}
	return -1;
}

//does not include text in user buffers
//does include the null byte
int line_length(int line) {
	if(b_get(line).longline == -1)
		return strlen(b_get(line).str) + 1;
	else {
		int length = LINE_LEN;
		int block = b_get(line).longline;

		while(b_get(block).longline != -1) {
			block = b_get(block).longline;
			length += LONG_LINE_LEN;
		}

		return length + strlen(b_get(block).str) + 1;
	}
}

int buf_count(int line) {
	int block = b_get(line).buffer;
	int c = 0;
	while(block != -1) {
		++c;
		block = b_get(block).next_buf;
	}
	return c;
}

int block_at(int line,int* offset) {
	int off = *offset;
	if(off < LINE_LEN)
		return line;
	off -= LINE_LEN;
	line = b_get(line).longline;
	while(off >= LONG_LINE_LEN ) {
		line = b_get(line).longline;
		off -= LONG_LINE_LEN;
	}
	*offset = off;
	return line;
}

void block_truncate(int line,int needed_len) {
	int block = line;

	needed_len -= LINE_LEN;

	while(needed_len > 0) {
		if(b_get(block).longline == -1) {
			int newblock = alloc_block();
			b_get(block).longline = newblock;
			b_get(newblock).longline = -1;
			block = newblock;
		} else
			block = b_get(block).longline;
		needed_len -= LONG_LINE_LEN;
	}

	int swp = block;
	block = b_get(swp).longline;
	b_get(swp).longline = -1;

	while(block != -1) {
		swp = block;
		block = b_get(swp).longline;
		free_block(swp);
	}
}

void copy_to_line(int line,int pos,char* src,int n) {
	int cpos = pos;
	int block = block_at(line,&cpos);

	memcpy(b_get(block).str + cpos,src,
			LENGTH_OF(line,block) - cpos < n ?
				LENGTH_OF(line,block) - cpos :
				n
		  );
	int i = LENGTH_OF(line,block) - cpos;
	block = b_get(block).longline;

	while(i + LONG_LINE_LEN < n) {
		memcpy(b_get(block).str,src + i,LONG_LINE_LEN);
		i += LONG_LINE_LEN;
		block = b_get(block).longline;
	}

	if(i < n)
		memcpy(b_get(block).str,src + i,n - i);
}

void copy_from_line(int line,int pos,char* dest,int n) {
	int cpos = pos;
	int block = block_at(line,&cpos);


	memcpy(dest,b_get(block).str + cpos,
			LENGTH_OF(line,block) - cpos < n ?
				LENGTH_OF(line,block) - cpos :
				n
		  );
	int i = LENGTH_OF(line,block) - cpos;
	block = b_get(block).longline;

	while(i + LONG_LINE_LEN < n) {
		memcpy(dest + i,b_get(block).str,LONG_LINE_LEN);
		i += LONG_LINE_LEN;
		block = b_get(block).longline;
	}

	if(i < n)
		memcpy(dest + i,b_get(block).str,n - i);
}

char* get_line_text(int line) {
	int line_len = line_length(line);
	//the size of the buffers may change, so allocate enough to hold the maximum size
	char *text = malloc(line_len + (LINE_LEN * buf_count(line)));

	int block;
	int total_pos = 0;
	int line_pos = 0;
	for(block = b_get(line).buffer;block != -1;block = b_get(block).next_buf) {
		copy_from_line(line,line_pos,text + total_pos,b_get(block).buf_pos - line_pos);
		total_pos += b_get(block).buf_pos - line_pos;
		line_pos = b_get(block).buf_pos;

		int buf_len = b_get(block).buf_used;//keep a local copy for consistency
		memcpy(text + total_pos,b_get(block).str,buf_len);
		total_pos += buf_len;
	}
	copy_from_line(line,line_pos,text + total_pos,line_len - line_pos);

	return text;
}

//always bounds check the return value when this is called without a write lock
int to_screen_pos(int line,int pos) {
	int spos = pos;
	int block = b_get(line).buffer;
	while(block != -1 && b_get(block).buf_pos < pos) {
		spos += b_get(block).buf_used;
		block = b_get(block).next_buf;
	}
	return spos;
}


//must have write lock
//pos must be >= 0 and < line_length, but dist can be any value
//this could be done in place, but the code for that would be much more complicated
void line_adjust(int line,int pos,int dist) {
	int length_past  = line_length(line) - pos;
	char tmp[length_past];

	int cpos = pos;
	int block = block_at(line,&cpos);
	
	if(b_get(block).longline == -1)
		memcpy(tmp,b_get(block).str + cpos,length_past);
	else {
		int i = LENGTH_OF(line,block) - cpos;
		memcpy(tmp,b_get(block).str + cpos,i);

		block = b_get(block).longline;
		while(b_get(block).longline != -1) {
			memcpy(tmp+i,b_get(block).str,LONG_LINE_LEN);
			block = b_get(block).longline;
			i += LONG_LINE_LEN;
		}

		memcpy(tmp+i,b_get(block).str,length_past - i);
	}

	if(pos + dist < 0) {//some text lost by moving before start of line
		int lost = -(pos + dist);
		if(lost >= length_past) {//all text lost from it
			block_truncate(line,1);
			b_get(line).str[0] = '\0';
		} else {
			block_truncate(line,length_past + lost);
			copy_to_line(line,0,tmp + lost,length_past - lost);
		}
	} else {
		block_truncate(line,length_past + pos + dist);
		copy_to_line(line,pos+dist,tmp,length_past);
	}

	//now update all cursors on the line
	
	int user;
	for(user = 0;user < USERS;++user) {
		if(data->line_at[user] == line) {
			int userpos = data->char_at[user];
			if(userpos > pos)
				data->char_at[user] += dist;
			else if(userpos > pos + dist)
				data->char_at[user] = pos + dist > 0 ? pos + dist : 0;
		}
	}

	int buf = b_get(line).buffer;

	while(buf != -1) {
		int userpos = b_get(buf).buf_pos;
		if(userpos > pos)
			b_get(buf).buf_pos += dist;
		else if(userpos > pos + dist)
			b_get(buf).buf_pos = pos + dist > 0 ? pos + dist : 0;
		buf = b_get(buf).next_buf;
	}


}

void flush_buf(int line,int buf) {
	int pos = b_get(buf).buf_pos;
	int len = b_get(buf).buf_used;
	line_adjust(line,pos,len);

	copy_to_line(line,pos,b_get(buf).str,len);
	b_get(buf).buf_used = 0;

	while(buf != -1 && b_get(buf).buf_pos == pos) {//all other buffers were handled by line_adjust
		data->char_at[b_get(buf).buf_owner] = b_get(buf).buf_pos += len;
		buf = b_get(buf).next_buf;
	}
}

int remove_buf(int line,int buf) {
	int block = -1;
	if(b_get(line).buffer == buf) 
		b_get(line).buffer = b_get(buf).next_buf;
	else {
		block = b_get(line).buffer;
		while(b_get(block).next_buf != buf)
			block = b_get(block).next_buf;
		b_get(block).next_buf = b_get(buf).next_buf;
	}
	free_block(buf);
	return block;
}

int add_buf(int line,int pos) {
	int block = b_get(line).buffer;
	int prev_buf = -1;

	while(block != -1 && b_get(block).buf_pos < pos) {
		prev_buf = block;
		block = b_get(block).next_buf;
	}

	int newblock = alloc_block();
	if(prev_buf == -1)
		b_get(line).buffer = newblock;
	else
		b_get(prev_buf).next_buf = newblock;

	b_get(newblock).buf_owner = user_no;
	b_get(newblock).next_buf = block;
	b_get(newblock).buf_pos = pos;
	b_get(newblock).buf_used = 0;

	return newblock;

}


/*
int prev_buf(int line,int buf) {
	int block = b_get(line).buffer;
	if(block == buf)
		return -1;
	while(b_get(block).next_buf != buf)
		block = b_get(block).next_buf;
	return block;
}
*/

void renumber_lines(int after) {
	int line;
	int num;
	if(after == -1) {
		line = data->first_line;
		num = 0;
	} else {
		line = b_get(after).next;
		num = b_get(after).lineno + 1;
	}
	while(line != -1) {
		b_get(line).lineno = num++;
		line = b_get(line).next;
	}
}

int relative_line(int line,int dis) {
	if(dis >= 0) {
		while(dis-- > 0 && b_get(line).next != -1)
			line = b_get(line).next;
	} else {
		while(dis++ < 0 && b_get(line).prev != -1)
			line = b_get(line).prev;
	}
	return line;
}

void draw_screen() {
	if(lines_from_top >= LINES)
		lines_from_top = LINES - 1;
	erase();
	read_lock();

	int user_screen_pos = to_screen_pos(data->line_at[user_no],data->char_at[user_no]) +
		(data->user_buf[user_no] == -1 ? 0 : b_get(data->user_buf[user_no]).buf_used);

	if(user_screen_pos < horizontal_scroll)
		horizontal_scroll = user_screen_pos;
	else if(user_screen_pos >= COLS + horizontal_scroll)
		horizontal_scroll = user_screen_pos - COLS + 1;


	int i;

	int top_line = data->line_at[user_no];
	i = 0;
	while(b_get(top_line).prev != -1 && i < lines_from_top) {
		top_line = b_get(top_line).prev;
		++i;
	}
	lines_from_top = i;

	int line = top_line;
	int window_line;
	for(window_line=0;window_line<LINES;++window_line) {
		if(line == -1) {
			break;
		}
		char* text = get_line_text(line);
		int len = strlen(text);

		if(len >= horizontal_scroll) {
			chtype display_text[len+1];
			for(i=0;i<len;++i)
				display_text[i] = text[i];

			display_text[len] = ' ';

			int adjust = 0;
			int block = b_get(line).buffer;
			while(block != -1) {
				adjust += b_get(block).buf_used;
			//	mvchgat(window_line,b_get(block).buf_pos + adjust,1,A_STANDOUT,0,NULL);
				if(b_get(block).buf_pos + adjust <= len)
					display_text[b_get(block).buf_pos + adjust] |= COLOR_PAIR(b_get(block).buf_owner + 1);
				block = b_get(block).next_buf;
			}

			for(i=0;i<USERS;++i) {
				if(data->line_at[i] == line &&
						data->user_buf[i] == -1) {
					int pos = to_screen_pos(line,data->char_at[i]);
					if(pos <= len) {
						if(!(display_text[pos] & (~A_CHARTEXT)))
							display_text[pos] |= COLOR_PAIR(i+1);
						else if(i == user_no)
							display_text[pos] = (display_text[pos] & A_CHARTEXT) | COLOR_PAIR(i+1);
					}
				}
			}

			mvaddchnstr(window_line,0,display_text+horizontal_scroll,len-horizontal_scroll+1);
		}
		free(text);
		line = b_get(line).next;
	}

	pthread_rwlock_unlock(&data->lock);

	refresh();//finally
}

//does not correctly handle files containing '\0'
int read_file(FILE *file) {

	char* tmp = NULL;
	size_t n = 0;
	int len;
	int firstline;

	int line = -1;
	while((len = getline(&tmp,&n,file)) != -1) {
		if(len > 0 && tmp[len-1] == '\n')
			tmp[len-1] = '\0';
		else
			len += 1;//for consistency with other parts of my code I am including the null byte in the length of a string

		int i;
		for(i=0;i<len-1;++i)
			if(tmp[i] == '\t')
				tmp[i] = ' ';

		int newline = alloc_block();
		b_get(newline).prev = line;
		if(line != -1)
			b_get(line).next = newline;
		else
			firstline = newline;

		b_get(newline).buffer = -1;
		b_get(newline).longline = -1;

		block_truncate(newline,len);
		copy_to_line(newline,0,tmp,len);

		line = newline;
	}

	free(tmp);

	if(line != -1)
		b_get(line).next = -1;

	return firstline;
}

int write_file(const char* fname) {
	FILE *file = fopen(fname,"w");
	if(file == NULL)
		return -1;

	read_lock();

	int line;

	for(line = data->first_line;line != -1;line = b_get(line).next) {
		char* text = get_line_text(line);
		fputs(text,file);
		fputc('\n',file);
		free(text);
	}
	pthread_rwlock_unlock(&data->lock);
	fclose(file);
	return 0;
}

void dump_debug(FILE* stream,bool get_lock) {
	if(get_lock)
		write_lock();

	fprintf(stream,"shm_name: %s\tuser_no: %d\tuser_buf: %d\tlines_from_top: %d\tfirst_free_block: %d\tblocks_avail: %d\n\n",shm_name,user_no,data->user_buf[user_no],lines_from_top,data->first_free_block,data->blocks_avail);

	int user;
	for(user=0;user<USERS;++user)
		fprintf(stream,"%d:\tline: %d\tchar: %d\tbuf: %d\n",user,data->line_at[user],data->char_at[user],data->user_buf[user]);

	int line = data->first_line;
	fprintf(stream,"first_line: %d\n",line);

	while(line != -1) {
		fprintf(stream,"lineno: %d\tblock: %d\tprev: %d\tnext: %d\n",b_get(line).lineno,line,b_get(line).prev,b_get(line).next);
		int block = b_get(line).longline;
		fprintf(stream,"longline:");
		while(block != -1) {
			fprintf(stream," %d",block);
			block = b_get(block).longline;
		}
		fprintf(stream,"\n");

		block = b_get(line).buffer;
		if(block != -1) {
			while(block != -1) {
				fprintf(stream,"user: %d\tblock: %d\tlen: %d\tpos: %d\t",b_get(block).buf_owner,block,b_get(block).buf_used,b_get(block).buf_pos);
				block = b_get(block).next_buf;
			}
			fprintf(stream,"\n");
		}
		line = b_get(line).next;
	}


	if(get_lock)
		pthread_rwlock_unlock(&data->lock);
}

void type_letter(char c) {
	read_lock();

	if(data->user_buf[user_no] == -1) {
		relock_write();
		int tmp = add_buf(data->line_at[user_no],data->char_at[user_no]);
		data->user_buf[user_no] = tmp;
	} else if(b_get(data->user_buf[user_no]).buf_used >= LINE_LEN) {
		relock_write();

		if(data->user_buf[user_no] == -1) {//a write may have happened after we released the read lock, so we need to check again
			int tmp = add_buf(data->line_at[user_no],data->char_at[user_no]);
			data->user_buf[user_no] = tmp;
		} else {
			flush_buf(data->line_at[user_no],data->user_buf[user_no]);
		}
	}
	int user_buf = data->user_buf[user_no];
	b_get(user_buf).str[b_get(user_buf).buf_used] = c;
	b_get(user_buf).buf_used += 1;
	pthread_rwlock_unlock(&data->lock);

}

//could be optimized, but it probably doesn't matter
void do_enter() {
	write_lock();

	int line_a = data->line_at[user_no];
	int line_b = alloc_block();

	b_get(line_b).longline = -1;
	b_get(line_b).buffer = -1;
	int line_c = b_get(line_b).next = b_get(line_a).next;
	b_get(line_b).prev = line_a;
	b_get(line_a).next = line_b;

	if(line_c != -1)
		b_get(line_c).prev = line_b;

	flush_line(line_a);
	int split = data->char_at[user_no];
	int len = line_length(line_a);

	char tmp[len - split];
	copy_from_line(line_a,split,tmp,len-split);
	block_truncate(line_a,split + 1);
	block_truncate(line_b,len - split);

	copy_to_line(line_b,0,tmp,len - split);
	copy_to_line(line_a,split,"",1);//put a null byte at the end of the line

	int user = 0;
	for(user=0;user<USERS;++user) {
		if(data->line_at[user] == line_a && data->char_at[user] > split) {
			data->line_at[user] = line_b;
			data->char_at[user] -= split;
		}
	}
	data->line_at[user_no] = line_b;
	data->char_at[user_no] = 0;

	renumber_lines(line_a);
	++lines_from_top;
	pthread_rwlock_unlock(&data->lock);
}

void flush_line(int line) {

	//first update all cursors on the line that are not editing
	int user;
	for(user = 0;user < USERS;++user) {
		if(data->line_at[user] == line && data->user_buf[user] == -1) {
			int block = b_get(line).buffer;
			int pos = data->char_at[user];
			while(block != -1 && pos > b_get(block).buf_pos) {
				data->char_at[user] += b_get(block).buf_used;
				block = b_get(block).next_buf;
			}
		}
	}


	int extra = 0;
	int block = b_get(line).buffer;
	while(block != -1) {
		extra += b_get(block).buf_used;
		block = b_get(block).next_buf;
	}
	int len = line_length(line);
	char tmp[len];
	copy_from_line(line,0,tmp,len);
	block_truncate(line,len+extra);

	int old_pos = 0;
	int new_pos = 0;
	block = b_get(line).buffer;
	while(block != -1) {
		int buf_pos = b_get(block).buf_pos;
		copy_to_line(line,new_pos,tmp+old_pos,buf_pos - old_pos);
		new_pos += buf_pos - old_pos;
		old_pos = buf_pos;
		int buf_len = b_get(block).buf_used;
		copy_to_line(line,new_pos,b_get(block).str,buf_len);
		new_pos += buf_len;

		//remove the buffer and adjust its owner
		int user = b_get(block).buf_owner;
		data->char_at[user] = new_pos;
		data->user_buf[user] = -1;

		int old_block = block;
		block = b_get(old_block).next_buf;
		free_block(old_block);
	}

	copy_to_line(line,new_pos,tmp+old_pos,len - old_pos);

	b_get(line).buffer = -1;
}

void empty_buf() {//there used to be a lot more to this function
	if(data->user_buf[user_no] != -1) {
		relock_write();
		flush_line(data->line_at[user_no]);
	}
}

//flush the line if there is an edit buf at the specified location
void flush_pos(int pos) {
	int block = b_get(data->line_at[user_no]).buffer;
	while(block != -1) {
		if(b_get(block).buf_pos > pos)
			break;
		else if(b_get(block).buf_pos == pos) {
			relock_write();
			flush_line(data->line_at[user_no]);
			break;
		}
		block = b_get(block).next_buf;
	}
}


void move_to_screen_pos(int line,int spos) {
	int len = line_length(line) - 1;
	int extra = 0;
	int block = b_get(line).buffer;
	while(block != -1) {
		if(spos - extra <= b_get(block).buf_pos)//the screen position fell outside any buffers
			break;
		else if(b_get(block).buf_pos == len  ||
				spos - extra <= b_get(block).buf_pos + b_get(block).buf_used) {//should this be <= or <

			data->line_at[user_no] = line;//it is possible that the line we intended to move to will be deleted after we release the lock
			relock_write();
			line = data->line_at[user_no];
			flush_line(line);
			len = line_length(line) - 1;
			extra = 0;
			break;
		}

		extra += b_get(block).buf_used;
		block = b_get(block).next_buf;
	}
	data->line_at[user_no] = line;
	data->char_at[user_no] = spos - extra > len ? len : spos - extra;
	return;
}

void move_left() {
	read_lock();
	empty_buf();
	if(data->char_at[user_no] > 0) {
		flush_pos(data->char_at[user_no] - 1);
		data->char_at[user_no] -= 1;
	}
	pthread_rwlock_unlock(&data->lock);
}

void move_right() {
	read_lock();
//	empty_buf();//we are at the location of our own edit buf, so flush_pos makes this unnecessary
	flush_pos(data->char_at[user_no]);

	if(data->char_at[user_no] < line_length(data->line_at[user_no]) -1)
		data->char_at[user_no] += 1;

	pthread_rwlock_unlock(&data->lock);
}

void move_up() {
	read_lock();
	empty_buf();
	int line = data->line_at[user_no];
	if(b_get(line).prev != -1) {
		--lines_from_top;
		move_to_screen_pos(b_get(line).prev,
				to_screen_pos(line,data->char_at[user_no]));
	}
	pthread_rwlock_unlock(&data->lock);
}

void move_down() {
	read_lock();
	empty_buf();
	int line = data->line_at[user_no];
	if(b_get(line).next != -1) {
		++lines_from_top;
		move_to_screen_pos(b_get(line).next,
				to_screen_pos(line,data->char_at[user_no]));
	}
	pthread_rwlock_unlock(&data->lock);
}

void move_end() {
	read_lock();
	empty_buf();
	move_to_screen_pos(data->line_at[user_no],INT_MAX);
	pthread_rwlock_unlock(&data->lock);
}

void move_home() {
	read_lock();
	empty_buf();
	flush_pos(0);
	data->char_at[user_no] = 0;
	pthread_rwlock_unlock(&data->lock);
}

void do_backspace() {
	read_lock();

	if(data->user_buf[user_no] == -1 || b_get(data->user_buf[user_no]).buf_used <= 1) {
		relock_write();

		//we just checked these conditions, but that was before we released the read lock
		if(data->user_buf[user_no] == -1) {
			if(data->char_at[user_no] == 0) {
				int line = data->line_at[user_no];

				int prev_line = b_get(line).prev;
				if(prev_line != -1) {
					int prev_len = line_length(prev_line) - 1;//line_length includes the ending null in its count
					int line_len = line_length(line);//this line will still have its ending null

					int prev_buf = b_get(prev_line).buffer;
					if(prev_buf != -1) {
						while(b_get(prev_buf).next_buf != -1)
							prev_buf = b_get(prev_buf).next_buf;

						b_get(prev_buf).next_buf = b_get(line).buffer;

						if(b_get(prev_buf).buf_pos == prev_len) {
							block_truncate(prev_line,prev_len + b_get(prev_buf).buf_used);
							copy_to_line(prev_line,prev_len,b_get(prev_buf).str,b_get(prev_buf).buf_used);
							prev_len += b_get(prev_buf).buf_used;

							int owner = b_get(prev_buf).buf_owner;
							data->char_at[owner] = prev_len;
							data->user_buf[owner] = -1;
							remove_buf(prev_line,prev_buf);
						}
					} else
						b_get(prev_line).buffer = b_get(line).buffer;

					int block = b_get(line).buffer;
					while(block != -1) {
						b_get(block).buf_pos += prev_len;
						block = b_get(block).next_buf;
					}

					int user;
					for(user = 0;user <USERS;++user) {
						if(data->line_at[user] == line) {
							data->line_at[user] = prev_line;
							data->char_at[user] += prev_len;
						}
					}

					char tmp[line_len];
					copy_from_line(line,0,tmp,line_len);
					block_truncate(prev_line,prev_len+line_len);
					copy_to_line(prev_line,prev_len,tmp,line_len);

					if(b_get(line).next != -1)
						b_get(b_get(line).next).prev = prev_line;

					b_get(prev_line).next = b_get(line).next;

					while(line != -1) {
						int swp = line;
						line = b_get(line).longline;
						free_block(swp);
					}
					renumber_lines(prev_line);
				}
			} else {
				int line = data->line_at[user_no];
				int pos = data->char_at[user_no];
				//try to half fill the buffer, but don't displace other users to do so
				int found = pos - (LINE_LEN/2) > 0 ? pos - (LINE_LEN/2) : 0;
				int user;
				for(user=0;user<USERS;++user) {
					if(user != user_no &&
							data->line_at[user] == line &&
							data->char_at[user] <= pos &&
							data->char_at[user] > found)
						found = data->char_at[user];
				}
				if(pos - found <= 1) {
					flush_line(line);
					line_adjust(line,data->char_at[user_no],-1);
				} else {
					int buf_init_size = pos - found;
					int buf = add_buf(line,pos);
					copy_from_line(line,found,b_get(buf).str,buf_init_size -1);//no need to copy the letter we are about to get rid of
					b_get(buf).buf_used = buf_init_size -1;
					data->user_buf[user_no] = buf;
					line_adjust(line,pos,-buf_init_size);
				}
			}

		} else {// b_get(data->user_buf[user_no]).buf_used <= 1
			remove_buf(data->line_at[user_no],data->user_buf[user_no]);
			data->user_buf[user_no] = -1;
		}
	} else {
		b_get(data->user_buf[user_no]).buf_used -= 1;
	}
	pthread_rwlock_unlock(&data->lock);
}

void handle_mouse() {
	MEVENT event;
	if(getmouse(&event) == OK && event.bstate == BUTTON1_PRESSED) {
		read_lock();
		int dest_line = relative_line(data->line_at[user_no],event.y - lines_from_top);
		lines_from_top = event.y;//FIXME: this produces interesting behavior when clicking below the bottom of the file
		empty_buf();
		move_to_screen_pos(dest_line,event.x + horizontal_scroll);
		pthread_rwlock_unlock(&data->lock);
	}
}

void clear_all() {
	int line = data->first_line;
	while(line != -1) {
		int block = b_get(line).buffer;
		while(block != -1) {
			int swp = block;
			block = b_get(block).next_buf;
			free_block(swp);
		}

		block = b_get(line).longline;
		while(block != -1) {
			int swp = block;
			block = b_get(block).longline;
			free_block(swp);
		}

		int swp = line;
		line = b_get(line).next;
		free_block(swp);
	}

	data->first_line = -1;

	int user;
	for(user = 0;user < USERS;++user) {
		if(data->line_at[user] >= 0){
			data->user_buf[user] = -1;
			data->char_at[user] = 0;
			data->line_at[user] = -1;
		}
	}
}

int dialog(const char* msg,char* input) {
	WINDOW* msg_win = newwin(5,NAME_LEN + 2,0,0);
	box(msg_win,0,0);
	mvwhline(msg_win,2,1,0,NAME_LEN);
	mvwaddstr(msg_win,1,1,msg);
	echo();
	mvwgetnstr(msg_win,3,1,input,NAME_LEN);
	noecho();
	delwin(msg_win);
	if(input[0] == '\0')
		return -1;
	else
		return 0;
}

void finish(void) {
	endwin();
	if(data != NULL && data != MAP_FAILED) {
		if(user_no >= 0)
			data->line_at[user_no] = NO_USER;
		//munmap here
		//done automagically for us on exit()
	}
	//I think exit() closes file descriptors for us
}

int main(int argc,char *argv[]) {
	umask(0);
	int i;
	bool create = false;
	bool debug = false;
	char* fname = NULL;
	for(i=1;i<argc;++i) {
		if(argv[i][0] == '-') {
			int k = 1;
			while(argv[i][k] != '\0') {
				switch(argv[i][k]) {
					case 'c':
						create = true;
						break;
					case 'd':
						debug = true;
						break;
					default:
						error(1,0,"Unrecognized flag: %c",argv[i][k]);
				}
				++k;
			}
		} else if(create && !fname)
			fname = argv[i];
		else if(!shm_name)
			shm_name = argv[i];
		else
			error(1,0,"Too many arguments");
	}

	if(shm_name == NULL)
		error(1,0,"No shared memory specified");
	if(create && fname == NULL)
		error(1,0,"No file specified");


	atexit(finish);

	if(!debug) {
		slk_init(3);
		initscr();
		start_color();
		use_default_colors();


#if DELAY
		halfdelay(DELAY);
#else
		nodelay(stdscr,true);
#endif
		noecho();
		keypad(stdscr,true);
		leaveok(stdscr,true);
		curs_set(0);

		mousemask(BUTTON1_PRESSED,NULL);
		mouseinterval(0);

		slk_set(1,"Debug",0);
		slk_set(2,"Save",0);
		slk_set(3,"Load",0);
		slk_set(10,"Quit",0);
		slk_noutrefresh();
	}



	shm_fd = shm_open(shm_name,
			O_RDWR | (create ? O_CREAT | O_TRUNC : 0),
			S_IWUSR |S_IRUSR|S_IWGRP |S_IRGRP|S_IWOTH|S_IROTH);


	if(shm_fd == -1)
		error_at_line(1,0,__FILE__,__LINE__,"Could not create or open shared memory");

	if(create)
		ftruncate(shm_fd,sizeof(struct Data));

	data = mmap(NULL,sizeof(struct Data),PROT_WRITE|PROT_READ,MAP_SHARED,shm_fd,0);
	if(data ==  MAP_FAILED)
		error_at_line(1,0,__FILE__,__LINE__,"Could not map shared memory");

	if(create) {
		init_data();
		FILE *file = fopen(fname,"r");
		if(file == NULL)
			error(1,0,"unable to open file \"%s\"",fname);

//		data->first_line = read_file(file);
		int tmp = read_file(file);
		data->first_line = tmp;
		fclose(file);
		renumber_lines(-1);
	}

	if((user_no = add_user()) < 0)
		error(1,0,"User limit reached for %s",shm_name);



	data->line_at[user_no] = data->first_line;
	data->char_at[user_no] = 0;

	if(debug) {
		recognize_growth();
		dump_debug(stderr,false);
		exit(0);
	}


	for(i=0;i<USERS;++i) {
		if(i == user_no)
			init_pair(i+1,COLOR_WHITE,(i%7)+1);
		else
			init_pair(i+1,(i%7)+1,COLOR_WHITE);
	}

	draw_screen();

	int key;
	while((key = getch()) != KEY_F(10)) {
		char name[NAME_LEN +1];
		FILE *file;
		switch(key) {
			case '\e'://gcc extension for esc key
				flushinp();
				break;
			case KEY_ENTER:
			case '\n':
				do_enter();
				break;
			case KEY_LEFT:
				move_left();
				break;
			case KEY_RIGHT:
				move_right();
				break;
			case KEY_UP:
				move_up();
				break;
			case KEY_DOWN:
				move_down();
				break;
			case KEY_BACKSPACE:
			case 127://?
				do_backspace();
				break;
			case KEY_MOUSE:
				handle_mouse();
				break;
			case KEY_F(1):
				if(dialog("Save debug info to what file?",name) != -1) {
					if((file = fopen(name,"w")) != NULL) {
						dump_debug(file,true);
						fclose(file);
					}
				}
				break;
			case KEY_F(2):
				if(dialog("Save as?",name) != -1)
					write_file(name);
				break;
			case KEY_F(3):
				if(dialog("Load what file? (this will erase any unsaved changes!)",name) != -1) {
					if((file = fopen(name,"r")) != NULL) {
						write_lock();
						clear_all();
						data->first_line = read_file(file);
						int user;
						for(user = 0;user<USERS;++user)
							if(data->line_at[user] == -1)
								data->line_at[user] = data->first_line;
						pthread_rwlock_unlock(&data->lock);
						fclose(file);
					}
				}
				break;
			case KEY_END:
				move_end();
				break;
			case KEY_HOME:
				move_home();
				break;
			case KEY_PPAGE:
				read_lock();
				empty_buf();
				move_to_screen_pos(
						relative_line(data->line_at[user_no],-LINES),
						to_screen_pos(data->line_at[user_no],data->char_at[user_no]));
				pthread_rwlock_unlock(&data->lock);
				break;
			case KEY_NPAGE:
				read_lock();
				empty_buf();
				move_to_screen_pos(
						relative_line(data->line_at[user_no],LINES),
						to_screen_pos(data->line_at[user_no],data->char_at[user_no]));
				pthread_rwlock_unlock(&data->lock);
				break;
			default:
				if(isprint(key))
					type_letter(key);
				break;
		}
		draw_screen();
	}
	read_lock();
	empty_buf();
	pthread_rwlock_unlock(&data->lock);

	return 0;
}

