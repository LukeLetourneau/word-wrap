#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>

#define DEBUG 1
#define printFailed 0

typedef enum bool { false = 0, true = 1 } bool;

int wrap(int fdr, int fdw, int line_length)
{
	int pstage = 0;
	char c[1];
	char word[5192];
	int wordlen = 0;
	int linelen = 0;
	int pos = 0;
	bool failed = false;
	bool ins_space = false;
	char lst, llst;
	while (read(fdr, c, 1)) {
		if (c[0] == ' ' || c[0] == '\n') { // Handle ws
			if (wordlen > 0) { // Add the word that's been being built first
				if (wordlen > line_length) { // The word is longer than the wrap length; report failure
					failed = true;
				}
				if (pos > 0 && linelen + wordlen + (ins_space ? 1 : 0) > line_length && lst != '\n') { // Word (when added) will exceed wrap length, so add newline (unless it is the first word)
					write(fdw, "\n", 1);
					pos++;
					linelen = 0;
					ins_space = false;
				}
				if(ins_space) {
					write(fdw, " ", 1);
					pos++;
					linelen++;
				}
				for (int i = 0; i < wordlen; i++) { // Add in the word
					write(fdw, word + i, 1);
					pos++;
					linelen++;
				}
				wordlen = 0;
			}
			if (pos == 0) continue;

			if (c[0] == '\n') {
				if(pstage == 1) { // Previous character was also newline, must make new paragraph
					write(fdw, "\n", 1);
					pos++;
					if(llst != '\n') {
						write(fdw, "\n", 1);
						pos++;
					}
					ins_space = false;
					linelen = 0;
					pstage = 2; // Directly proceding newlines are ignored
				} else if(pstage == 0) {
					pstage = 1;
					ins_space = true;
				}
			} else if (c[0] == ' ') {
				if (pos != 0 && lst != ' ' && lst != '\n' && linelen <= line_length) { // Add space char if appropriate: no preceding whitespace and not overrunning line length
					ins_space = true;
				}
			}
		} else {
			if(pstage == 1) {
				if(c[0] != ' ' && lst != ' ' && lst != '\n') { // Add a space instead of the newline, if it's needed
					write(fdw, " ", 1);
					pos++;
					linelen++;
				}
			}
			pstage = 0;
			word[wordlen] = c[0];
			wordlen++;
		}
		llst = lst;
		lst = c[0];
	}
	if (wordlen > 0) { // Repeat of above, add word into file in case EOF with word in buffer
		if (wordlen > line_length) {
			failed = true;
		}
		if (linelen + wordlen > line_length) {
			write(fdw, "\n", 1);
			linelen = 0;
			pos++;
		}
		for (int i = 0; i < wordlen; i++) {
			write(fdw, word + i, 1);
			pos++;
			linelen++;
		}
		wordlen = 0;
	}
	if(pos > 0) {
		write(fdw, "\n", 1);
		pos++;
	}
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

typedef struct node
{
    char* path;
    struct node* next;
} node;

typedef struct queue {
    node* head;
    node* rear;
    int open;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t enqueue_ready, dequeue_ready;
} queue;

void queue_init(queue *q)
{
    if(DEBUG)printf("queue Initialized\n");
    q->head = NULL;
    q->rear = NULL;
    q->open = 0;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->dequeue_ready, NULL);
}

void printQueue(queue *q)
{
    printf("------------------------------------\n");
    pthread_mutex_lock(&q->lock);
    if(DEBUG)printf("locked \n");
	struct stat st;
    for(node* temp = q->head; temp!=NULL; temp=temp->next)
    {
		stat(temp->path, &st);
        if(S_ISDIR(st.st_mode))
        {
            printf("dirQueue?: |%s| \n", temp->path);
        }
        else if(S_ISREG(st.st_mode))
        {
            printf("fileQueue?: |%s| \n", temp->path);
        }
    }
    if(q->head == NULL)
    {
        printf("Nothing in Queue\n");
    }
    if(DEBUG)printf("unlocked \n");
    pthread_mutex_unlock(&q->lock);
    printf("------------------------------------\n");
}

void enqueue(node* n, queue *q)
{
    if(q->head == NULL)
    {
        q->head = n;
        q->rear = n;
    }
    else
    {
        q->rear->next = n;
        q->rear = n;
    }
    q->count++;
    pthread_cond_signal(&q->dequeue_ready);

}
void dequeue(node** n, queue *q)
{
    *n = q->head;
    q->head = q->head->next;
    q->count--;
}

typedef struct dwargs {
	int tid;
	queue* dq;
	queue* fq;
} dwargs;


//I dont think its a good idea to unlock before dequeue
void* directoryworker(void* vargs) {
	printf("Started dir worker thread\n");
	int *ret;
	int out = 0;
	ret = &out;
	dwargs* args = (dwargs*)vargs;
    while(true) {
		//printQueue(args->dq);
		//printf("%d: waiting for dir to dequeue\n", args->tid);
		//if(DEBUG)printf("   %d: lock dq\n", args->tid);
		pthread_mutex_lock(&args->dq->lock);
		//printf("%d: %d open and %d total\n", args->tid, args->dq->open, args->dq->count);
		if(args->dq->open == 0 && args->dq->count == 0) {
			//printf("%d: no more dirs\n", args->tid);
			//if(DEBUG)printf("   %d: lock fq\n", args->tid);
			pthread_cond_signal(&args->dq->dequeue_ready);
			pthread_mutex_unlock(&args->dq->lock);
			pthread_mutex_lock(&args->fq->lock);
			pthread_cond_signal(&args->fq->dequeue_ready);
			//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
			//if(DEBUG)printf("   %d: unlock fq\n", args->tid);
			pthread_mutex_unlock(&args->fq->lock);
			pthread_exit((void*)ret);
			return (void*)ret;
		}
		while(args->dq->count == 0 && args->dq->open > 0) {
			//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
			//printf("   %d: waiting\n", args->tid);
			pthread_cond_wait(&args->dq->dequeue_ready, &args->dq->lock);
			//if(DEBUG)printf("   %d: lock dq\n", args->tid);
		}
		//printf("%d: %d open and %d total\n", args->tid, args->dq->open, args->dq->count);
		if(args->dq->open == 0 && args->dq->count == 0) {
			//printf("%d: no more dirs\n", args->tid);
			//if(DEBUG)printf("   %d: lock fq\n", args->tid);
			pthread_cond_signal(&args->dq->dequeue_ready);
			pthread_mutex_unlock(&args->dq->lock);
			pthread_mutex_lock(&args->fq->lock);
			pthread_cond_signal(&args->fq->dequeue_ready);
			//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
			//if(DEBUG)printf("   %d: unlock fq\n", args->tid);
			pthread_mutex_unlock(&args->fq->lock);
			pthread_exit((void*)ret);
			return (void*)ret;
		}
		//printQueue(args->dq);
		//if(DEBUG)printf("%d: there is a dir to read \n", args->tid);

		//chech for cond here
		node* deqNode = (node*)malloc(sizeof(node));
    	dequeue(&deqNode, args->dq);
		args->dq->open++;
		//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
    	pthread_mutex_unlock(&args->dq->lock);

		if(deqNode==NULL) 
		{
        	if(DEBUG) printf("there is something wrong\n");
			pthread_mutex_unlock(&args->dq->lock);
			return ret;
    	}
		//printf("owo\n");
		struct stat st;
		stat(deqNode->path, &st);
		if(S_ISREG(st.st_mode)) {
			//printf("file in dq\n");
			node* n = (node*)malloc(sizeof(node));
			n->path = deqNode->path;
			n->next = NULL;
			//if(DEBUG)printf("   %d: lock fq\n", args->tid);
			pthread_mutex_lock(&args->fq->lock);
			enqueue(n, args->fq);
			pthread_cond_signal(&args->fq->dequeue_ready);
			//if(DEBUG)printf("   %d: unlock fq\n", args->tid);
			pthread_mutex_unlock(&args->fq->lock);

			//if(DEBUG)printf("   %d: lock dq\n", args->tid);
			pthread_mutex_lock(&args->dq->lock);
    		args->dq->open--;
			//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
    		pthread_mutex_unlock(&args->dq->lock);
			continue;
		}
		//printf("not a file lol\n");
    	//if(DEBUG) printf("here dequeded %s\n", deqNode->path);

    	DIR* qdir = opendir(deqNode->path);

    	if(qdir == NULL)
    	{
        	printf("invalid directory in queue of |%s| \n", deqNode->path);
    	}
		//printf("hehe\n");
    	struct dirent *cdir;
    	cdir = readdir(qdir);

		//printf("awa\n");
    	while(cdir!=NULL)
    	{
			//printf("uwu\n");
        	int ppathlen = strlen(deqNode->path);
        	int cpathlen = strlen(cdir->d_name);
        	char* cpath = (char*)malloc(ppathlen + cpathlen + 2);
        	memcpy(cpath, deqNode->path, ppathlen);
        	cpath[ppathlen] = '/';
        	memcpy(&cpath[ppathlen + 1], cdir->d_name, cpathlen);
        	cpath[ppathlen + 1 + cpathlen] = '\0';

        	stat(cpath, &st);
        	if(cpathlen == 0 || cdir->d_name[0] == '.' || !strncmp("wrap.", cdir->d_name, 5))
        	{
				//if(DEBUG) printf("ignored |%s| \n",  cdir->d_name);
        	} else if(S_ISDIR(st.st_mode)) { //check if a directory is a folder
            	//if(1)printf("adding |%s| to dirQueue\n",newDir);
            	node* n = (node*)malloc(sizeof(node));
				n->path = cpath;
				n->next = NULL;

				//if(DEBUG)printf("   %d: lock dq\n", args->tid);
				pthread_mutex_lock(&args->dq->lock);
				enqueue(n, args->dq);
				//printf("%s\n", n->path);
				pthread_cond_signal(&args->dq->dequeue_ready);
				//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
				pthread_mutex_unlock(&args->dq->lock);

        	} else if(S_ISREG(st.st_mode)) { //Check if a directory is file
            	//if(1)printf("adding |%s| as dirName and |%s| as fileName to fileQueue\n",deqNode->path, cdir->d_name);
            	node* n = (node*)malloc(sizeof(node));
				n->path = cpath;
				n->next = NULL;

				//if(DEBUG)printf("   %d: lock fq\n", args->tid);
				pthread_mutex_lock(&args->fq->lock);
				enqueue(n, args->fq);
				pthread_cond_signal(&args->fq->dequeue_ready);
				//if(DEBUG)printf("   %d: unlock fq\n", args->tid);
				pthread_mutex_unlock(&args->fq->lock);
        	}
        	cdir = readdir(qdir);
    	}
    	closedir(qdir);

		//if(DEBUG)printf("   %d: lock dq\n", args->tid);
    	pthread_mutex_lock(&args->dq->lock);
    	//if(DEBUG)printf("locked \n");
    	args->dq->open--;
    	//if(DEBUG)printf("unlocked \n");
		//if(DEBUG)printf("   %d: unlock dq\n", args->tid);
    	pthread_mutex_unlock(&args->dq->lock);
		//printf("read dir\n");
    }
    //if(DEBUG)printf("unlocked \n");
    //if(locked == 0) pthread_mutex_unlock(&args->dq->lock);
    return (void*)ret;
}

typedef struct fwargs {
	int tid;
	queue* fq;
	queue* dq;
	int line_len;
} fwargs;

void* fileworker(void* vargs) {
	fwargs* args = (fwargs*)vargs;
    //int locked = pthread_mutex_trylock(&args->fq->lock);
    printf("Started file worker thread |%d|\n",args->tid);
	int *ret;
	int out = 0;
	ret = &out;
	char dirsempty = 0;
	while(1) {
		//printf("im working--------------------w  aiting for file to dequeue\n");
		pthread_mutex_lock(&args->dq->lock);
		if(args->dq->count == 0 && args->dq->open == 0) dirsempty = 1;
		pthread_mutex_unlock(&args->dq->lock);
		pthread_mutex_lock(&args->fq->lock);
		//printf("im working--------------------g  ot through locks\n");
		if(args->fq->count == 0 && dirsempty == 1) {
			pthread_cond_signal(&args->fq->dequeue_ready);
			pthread_mutex_unlock(&args->fq->lock);
			printf("%d: exiting\n", args->tid);
			pthread_exit((void*)ret);
			return (void*)ret;
		}
		while(args->fq->count == 0 && args->fq->open > 0) {
			//pthread_mutex_lock(&args->dq->lock);
			/*if(args->dq->count == 0 && args->dq->open == 0) {
				printf("file worker done\n");
				pthread_cond_signal(&args->fq->dequeue_ready);
				pthread_mutex_unlock(&args->dq->lock);
				pthread_mutex_unlock(&args->fq->lock);
				pthread_exit((void*)ret);
				return NULL;
			}*/
			//pthread_mutex_unlock(&args->dq->lock);
			printf("fw waiting\n");
			pthread_cond_wait(&args->fq->dequeue_ready, &args->fq->lock);
			printf("%d", args->fq->count);
		}
		if(args->fq->count == 0) {
			pthread_cond_signal(&args->fq->dequeue_ready);
			pthread_mutex_unlock(&args->fq->lock);
			printf("%d: wexiting\n", args->tid);
			pthread_exit((void*)ret);
			return (void*)ret;
		}
		args->fq->open++;
    	node* deqNode = (node*)malloc(sizeof(node));
    	dequeue(&deqNode, args->fq);
		pthread_mutex_unlock(&args->fq->lock);

		char wrappre[5] = "wrap.";
		int pathlen = strlen(deqNode->path), ls;
		char* writepath = (char*)malloc((pathlen + 6));
		for(int i = 0; i < pathlen; i++) {
			writepath[i] = deqNode->path[i];
			if(writepath[i] == '/') ls = i;
		}
		memcpy(&writepath[ls + 1], &wrappre, 5);
		memcpy(&writepath[ls + 6], &deqNode->path[ls + 1], pathlen - ls + 2); // Does this add \0?
		//writepath[pathlen + 6] = '\0';

    	printf("open file |%s| and write to |%s|\n", deqNode->path, writepath);
    	int fdr = open(deqNode->path, O_RDONLY);
    	int fdw = open(writepath, O_RDWR | O_CREAT |O_TRUNC, S_IRUSR|S_IWUSR);
    	*ret = *ret | wrap(fdr, fdw, args->line_len);

    	close(fdr);
    	close(fdw);
    	free(deqNode);
		free(writepath);

    	pthread_mutex_lock(&args->fq->lock);
    	args->fq->open--;
    	pthread_mutex_unlock(&args->fq->lock);
	}
    return (void*)ret;
}

/*void dtoq(char* dpath, queue* q) {
	struct stat st;
	stat(dpath, &st);
	DIR* dir = opendir(dpath);

   	struct dirent *cdir;
   	cdir = readdir(dir);

   	while(cdir!=NULL) {
       	int ppathlen = strlen(dpath);
       	int cpathlen = strlen(cdir->d_name);
       	char* cpath = (char*)malloc(ppathlen + cpathlen + 2);
       	memcpy(cpath, dpath, ppathlen);
       	cpath[ppathlen] = '/';
       	memcpy(&cpath[ppathlen + 1], cdir->d_name, cpathlen);
       	cpath[ppathlen + 1 + cpathlen] = '\0';
        stat(cpath, &st);
       	if(cpathlen == 0 || cdir->d_name[0] == '.' || !strncmp("wrap.", cdir->d_name, 5))
       	{

       	} else if(S_ISREG(st.st_mode)) { //Check if a directory is file
           	node* n = (node*)malloc(sizeof(node));
			n->path = cpath;
			n->next = NULL;

			pthread_mutex_lock(&q->lock);
			enqueue(n, q);
			pthread_cond_signal(&q->dequeue_ready);
			pthread_mutex_unlock(&q->lock);
       	}
       	cdir = readdir(dir);
   	}
   	closedir(dir);
}
*/
int main(int argc, char** argv) {
    if(argc < 3) {
        printf("ERROR: Invalid number of parameters\n");
        return EXIT_FAILURE;
    }

	queue* dq = (queue*)malloc(sizeof(queue));
    queue_init(dq);
    queue* fq = (queue*)malloc(sizeof(queue));
    queue_init(fq);
	int line_length;
	int M = 1;
    int N = 1;
	if(strncmp(argv[1],"-r",2) != 0)
	{
		printf("not recursive \n");
		if(argc == 3)
		{
			execl("./ww",argv[1],argv[2]);
			printf("ERROR: Failed execl\n");
			return EXIT_FAILURE;
		}
		struct stat st;
		for(int i = 2; i<argc; i++)
		{
			stat(argv[i], &st);
			if(S_ISDIR(st.st_mode))
        	{
				DIR *givenDir = opendir(argv[i]);
    			struct dirent *currDir;
    			currDir = readdir(givenDir);
				struct stat checkDir;
				while(currDir!=NULL)
    			{
					int dirLen = strlen(argv[i]);
					int cfLen = strlen(currDir->d_name);
					char* cpath = (char*)malloc(dirLen + cfLen +2);
					memcpy(cpath, argv[i], dirLen);
					memcpy(&cpath[dirLen], "/", 1);
					memcpy(&cpath[dirLen+1], currDir->d_name, cfLen);
					memcpy(&cpath[dirLen+1+cfLen], "\0", 1);
					stat(cpath, &checkDir);	
					int cpathlen = strlen(currDir->d_name);
					if(cpathlen == 0 || currDir->d_name[0] == '.' || !strncmp("wrap.", currDir->d_name, 5))
        			{
						//printf("ignored |%s| \n",  currDir->d_name);
        			}
					else if(S_ISREG(checkDir.st_mode))
					{
						//printf("added |%s| \n",  cpath);
						node* fileAdd = (node*)malloc(sizeof(node));
						fileAdd->path = cpath;
						fileAdd->next = NULL;
            			enqueue(fileAdd, fq);
					}
					currDir = readdir(givenDir);
				}	
       		}
        	else if(S_ISREG(st.st_mode))
        	{
			node* fileAdd = (node*)malloc(sizeof(node));
			fileAdd->path = argv[i];
			fileAdd->next = NULL;
            enqueue(fileAdd, fq);
        	}
		}
	}
	else
	{
		printf("recursive \n");
		int comma = -1;
    	int strlength = strlen(argv[1]);
    	line_length = atoi(argv[2]);
    	char* dirPath = argv[3];
    	if(strlength==2)
    	{
        	M = 1;
        	N = 1;
        	if(DEBUG) printf("running -r1,1 \n");
    	}
		else
    	{
        	int i;
        	for(i = 2; i < strlength;i++)
        	{
            	if(argv[1][i]==',')
            	{
                	comma = i;
                	break;
            	}
        	}
        	if(comma == -1)
        	{
            	char* Ntemp = (char*)malloc(sizeof(char)*(strlength-1));
            	memcpy(Ntemp,&argv[1][2],strlength-1);
            	N = atoi(Ntemp);
            	if(DEBUG) printf("running -r%d since N is %d \n", N, N);
            	free(Ntemp);
        	}
        	else
        	{
            	char* Mtemp = (char*)malloc(sizeof(char)*(i-1));
            	char* Ntemp = (char*)malloc(sizeof(char)*(strlength-i+1));
            	memcpy(Mtemp,&argv[1][2],i-1);
            	memcpy(Ntemp,&argv[1][i+1],strlength-i+1);
            	Mtemp[i-1] = '\0';
            	M = atoi(Mtemp);
            	N = atoi(Ntemp);
            	if(DEBUG) printf("running -r%d,%d since M is %d and N is %d \n", M,N, M,N);
            	free(Mtemp);
            	free(Ntemp);
        	}
		}
		struct stat st;
		for(int j = 3; j<argc; j++)
		{
			stat(argv[j], &st);
			if(S_ISDIR(st.st_mode))
        	{
            	node* dirAdd = (node*)malloc(sizeof(node));
				dirAdd->path = argv[j];
				dirAdd->next = NULL;
            	enqueue(dirAdd, dq);
       		}
        	else if(S_ISREG(st.st_mode))
        	{
				node* fileAdd = (node*)malloc(sizeof(node));
				fileAdd->path = argv[j];
				fileAdd->next = NULL;
            	enqueue(fileAdd, fq);
        	}
		}
    }


	
	// int recursive;
	// int i = recursive ? 3 : 2;
	// // Or i = 2 if not recursive
	// for(; i < argc; i++) {
	// 	node* f = malloc(sizeof(node));
	// 	f->path = argv[i];
	// 	f->next = NULL;
	// 	enqueue(f, dq);
	// }

	printQueue(dq);
	printQueue(fq);
	/////////////////////////
	//abort();
	/////////////////////////

	pthread_t* dwtids;
	dwargs** da;
	// Start Directory worker threads
	dwtids = malloc(M * sizeof(pthread_t));
	da = malloc(M * sizeof(dwargs*));
	for(int i = 0; i < M; i++) {
		dwargs* di = malloc(sizeof(dwargs));
		di->dq = dq;
		di->fq = fq;
		di->tid = i;
		da[i] = di;
	}

	for(int i = 0; i < M; i++) {
		pthread_create(&dwtids[i], NULL, directoryworker, da[i]);
	}

	//Start Fileworker Threads
	pthread_t* fwtids = malloc(N * sizeof(pthread_t));
	fwargs* fa = malloc(sizeof(fwargs));
	fa->fq = fq;
	fa->dq = dq;
	fa->line_len = line_length;
	for(int i = 0; i < N; i++) {
		pthread_create(&fwtids[i], NULL, fileworker, fa);
	}

	printf("All threads started-------------------------------------------------\n");

	unsigned int status = 0;
	unsigned int* s;
	// Join threads
	for(int i = 0; i < M; i++) {
		pthread_join(dwtids[i], NULL);
		//status = status | *s;
		printf("joined %d\n", i);
	}

	printf("Dir workers done\n");

	for(int i = 0; i < N; i++) {
		pthread_join(fwtids[i], NULL);
		//printf("%d\n", *s);
		//status = status | *s;
	}

	int ko = 100;
	printf("%d\n", ko);
	printf("???\n");
	printf("File workers done\n");
	return status;

}
