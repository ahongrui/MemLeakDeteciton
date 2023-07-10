#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <time.h>
#include <signal.h>
#include <malloc.h>
#include <sys/mman.h>
#define PRT printf
#define TABLE_SIZE	98317UL
#define MAX_STACK_DEPTH 32
#define RPT_SIGNAL 64
#define DEF_PAGE_SIZE 4096
#define DEF_OUTPUT_PATH 	"./"

#if (define (__x86_64__) || define (__aarch64__))
#define FLAG_VALUE 0XFFFFFFFFFFFFFFFF
#else
#define FLAG_VALUE 0XFFFFFFFF
#endif

extern "C" void* __libc_malloc(size_t bytes);
extern "C" void* __libc_free(void* mem);
extern "C" void* __libc_calloc(size_t n, size_t elem_size);
extern "C" void* __libc_realloc(void* __ptr, size_t __size);
extern "C" void* __libc_memalign(size_t __alignment, size_t __size);
// �ڴ��������
typedef void* (*FUNC_MALLOC)(unsigned int size);
typedef void* (*FUNC_CALLOC)(size_t num, size_t size);
typedef void* (*FUNC_REALLOC)(void *mem_address, size_t newsize);
typedef void* (*FUNC_VALLOC)(size_t size);
typedef void* (*FUNC_PVALLOC)(size_t size);
typedef int (*FUNC_POSIX_MEMALIGN)(void **memptr, size_t alignment, size_t size);
typedef void* (*FUNC_MEMALIGN)(size_t boundary, size_t size);
typedef void* (*FUNC_ALIGNED_ALLOC)(size_t alignment, size_t size);
typedef void (*FUNC_FREE)(void *ptr);
static FUNC_MALLOC g_pfnMalloc = NULL;
static FUNC_CALLOC g_pfnCalloc = NULL;
static FUNC_REALLOC g_pfnRealloc = NULL;
static FUNC_VALLOC g_pfnValloc = NULL;
static FUNC_PVALLOC g_pfnPvalloc = NULL;
static FUNC_POSIX_MEMALIGN g_pfnPosixMemalign = NULL;
static FUNC_MEMALIGN g_pfnMemalign = NULL;
static FUNC_ALIGNED_ALLOC g_pfnAlignAlloc = NULL;
static FUNC_FREE g_pfnFree = NULL;

// ����backtrace����
static int(*g_pfnBackTrace)(void** stack, int size);

// ��ɳ�ʼ��ǰmalloc��free����ҵ��ͳ��
static bool g_bInitFlag = false;

// ��ֹ�ڴ����������ѭ��
thread_local static int g_tlsKey = 0;

//��ջ��Ϣ�ṹ��
struct BtInfo
{
	struct BtInfo* next;			//��������
	int		requestLength;			//�û�������ڴ��С
	int		extraLength;			//�������û��ڴ�ǰ����չ���ڴ泤��
	int		count;					//ջ���ø���
	int		depth;					//ջ���
	size_t hash;					//hash
	void* bt[MAX_STACK_DEPTH];		//���ε�ջ֡��ַ
};
//�ڴ�ǰ�ýṹ��
struct PtrRep
{
	BtInfo* stack;
	size_t  flag;
};

//��ϣ��洢��ջ��Ϣ
static BtInfo* g_hashTable[TABLE_SIZE] = { 0 };

/*  gcc���ú��� */
/***********************************************/
#define GCCBT(i) \
    if(i >= size) return i;\
    frameNow = __builtin_frame_address(i);\
    if((unsigned long)frameNow <= (unsigned long)frameLast ||\
    ((unsigned long)frameLast != 0 && (unsigned long)frameNow > (unsigned long)frameLast + (1ULL<<24)))\
        return i;\
    frameLast = frameNow;\
    stack[i] = __builtin_extract_return_addr(__builtin_return_address(i));\

static int gccBacktrace(void** stack, int size)
{
	void* frameNow = 0;
	void* frameLast = 0;
	GCCBT(0); GCCBT(1); GCCBT(2); GCCBT(3); GCCBT(4); GCCBT(5); GCCBT(6); GCCBT(7);
	GCCBT(8); GCCBT(9); GCCBT(10); GCCBT(11); GCCBT(12); GCCBT(13); GCCBT(14); GCCBT(15);
	GCCBT(16); GCCBT(17); GCCBT(18); GCCBT(19); GCCBT(20); GCCBT(21); GCCBT(22); GCCBT(23);
	GCCBT(24); GCCBT(25); GCCBT(26); GCCBT(27); GCCBT(28); GCCBT(29); GCCBT(30); GCCBT(31);
	return 32;
}

/*  ���û���ȡ�Ĵ�������ֵ */
/***********************************************/
static int asmBacktrace(void** stack, int size)
{
#ifndef __x86_64__
	return 0;
#else
	int frame = 0;
	void ** ebp;
	unsigned long long func_frame_distance = 0;
	__asm__ __volatile__("mov %%rbp, %0;\n\t" : "=m"(ebp) ::"memory");
	while (ebp && frame < size
		&& (unsigned long long)(*ebp) < (unsigned long long)(ebp) + (1ULL << 24)//16M
		&& (unsigned long long)(*ebp) > (unsigned long long)(ebp))
	{
		stack[frame++] = *(ebp + 1);
		ebp = (void**)(*ebp);
	}
	return frame;
#endif
}

// ��ʼ������
static void InitMemFunc()
{
	g_pfnMalloc = (FUNC_MALLOC)dlsym(RTLD_NEXT, "malloc");
	g_pfnFree = (FUNC_FREE)dlsym(RTLD_NEXT, "free");
	g_pfnCalloc = (FUNC_CALLOC)dlsym(RTLD_NEXT, "calloc");
	g_pfnRealloc = (FUNC_REALLOC)dlsym(RTLD_NEXT, "realloc");
	g_pfnValloc = (FUNC_VALLOC)dlsym(RTLD_NEXT, "valloc");
	g_pfnPvalloc = (FUNC_PVALLOC)dlsym(RTLD_NEXT, "pvalloc");
	g_pfnMemalign = (FUNC_MEMALIGN)dlsym(RTLD_NEXT, "memalign");
	g_pfnPosixMemalign = (FUNC_POSIX_MEMALIGN)dlsym(RTLD_NEXT, "posix_memalign");
	g_pfnAlignAlloc = (FUNC_ALIGNED_ALLOC)dlsym(RTLD_NEXT, "aligned_alloc");
}

// ����
static int	g_maxStackDepth = MAX_STACK_DEPTH; 	//btջ���
static char g_btType[10] = "unw";				//bt��ʽ��libc/gcc/asm/unw
static int 	g_rptSignal = RPT_SIGNAL;			//�û�֪ͨ�ź�
static char g_rptPath[1024] = {0};				//Ĭ�����·��
static char g_rptFile[128] = { 0 };				//����ļ�����
static char g_rptExePath[1024] = { 0 };			//ִ���ļ�����·��
static char g_libunwindPath[1024] = {0}; 		//libunwind.so·��
static size_t g_pageSize = DEF_PAGE_SIZE;		//�ڴ����ҳ��Ĭ��ֵ

static void InitConfig()
{
	//ҳ��size
	int tmpPageSize = getpagesize();
	g_pageSize = tmpPageSize > 0 ? tmpPageSize : g_pageSize;
	
	//��ջ���
	char* strEnv = getenv("MAL_MAX_STACK_DEPTH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		int iDepth = atoi(strEnv);
		g_maxStackDepth = (iDepth > 0 && iDepth < MAX_STACK_DEPTH) ? iDepth :  g_maxStackDepth;
	}
	
	//bt��ʽ
	strEnv = getenv("MAL_BACKTRACE_TYPE");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_btType, strEnv, sizeof(g_btType) - 1);
	}
	
	//֪ͨ�ź�
	strEnv = getenv("MAL_REPORT_SIGNAL");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		int iSignal = atoi(strEnv);
		g_rptSignal = (iSignal > 0 && iSignal <= 64) ? iSignal :  g_rptSignal;
	}
	
	//���·��
	strEnv = getenv("MAL_REPORT_PATH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_rptPath, strEnv, sizeof(g_rptPath) - 1);
	}
	else
	{
		strncpy(g_rptPath, DEF_OUTPUT_PATH, sizeof(g_rptPath) - 1);
	}

	//����ļ�����
	readlink("/proc/self/exe", g_rptExePath, sizeof(g_rptExePath) - 1);
	int length = strlen(g_rptExePath) - 1;
	while (g_rptExePath[length] != '/' && length > 0)
	{
		--length;
	}
	snprintf(g_rptFile, sizeof(g_rptFile), "pid_%llu_proc_%s.bt",getpid(), (char*)(g_rptExePath+length+1));
	
	//libunwind.so����·��
	strEnv = getenv("MAL_LIBUNWIND_PATH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_libunwindPath, strEnv, sizeof(g_libunwindPath) - 1);
	}
	else
	{
		strncpy(g_libunwindPath, "./libunwind.so", sizeof(g_libunwindPath) - 1);
	}
}

static void InitBtFunc()
{
	if (strcmp(g_btType, "gcc") == 0)
	{
		g_pfnBackTrace = gccBacktrace;
	}
	else if (strcmp(g_btType, "libc") == 0)
	{
		g_pfnBackTrace = backtrace;
	}	
	else if (strcmp(g_btType, "asm") == 0)
	{
		g_pfnBackTrace = asmBacktrace;
	}	
	else if (strcmp(g_btType, "unw") == 0)
	{
		void* handle = dlopen(g_libunwindPath, RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND);
		if (handle == nullptr)
		{
			g_pfnBackTrace = gccBacktrace;
			PRT("dlopen failed, %s\n",dlerror());
			return;
		}
		g_pfnBackTrace = (int(*)(void**, int))dlsym(handle, "unw_backtrace");
		if (g_pfnBackTrace == nullptr)
		{
			g_pfnBackTrace = gccBacktrace;
			PRT("dlsym failed, %s\n", dlerror());
			return;
		}
	}	
	else
	{
		g_pfnBackTrace = gccBacktrace; //Ĭ��ʹ��backtrace
	}
}

static bool IsExeFile(const char* fname)
{
	bool bRet = true;
	int lenSoName = strlen(fname);
	int lenExePath = strlen(g_rptExePath);
	while (lenSoName-- >= 0 && lenExePath-- >= 0)
	{
		if (g_rptExePath[lenExePath] == '/' || fname[lenSoName] == '/')
		{
			break;
		}
		if (g_rptExePath[lenExePath] != fname[lenSoName])
		{
			bRet = false;
			break;
		}
	}
	return bRet;
}

//д�뵥��ջ��btͳ����Ϣ
static void RecordSingleStack(BtInfo* bp, FILE* fp)
{
	fprintf(fp,"count:%d size:%d", bp->count, bp->requestLength);
	for (int i = 0; i < bp->depth; ++i)
	{
		void* ptr = bp->bt[i];
		Dl_info info = {0};
		if (dladdr(ptr, &info) == 0)
		{
			fprintf(fp, " unknow:%p", ptr);
			continue;
		}
		size_t iAddr = 0;
		if (IsExeFile(info.dli_fname))
		{
			iAddr = (size_t)ptr;
		}
		else
		{	//����ǹ���⣬Ҫ��ȥƫ��
			iAddr = (size_t)ptr - (size_t)info.dli_fbase;
		}
		fprintf(fp, " %s:0x%x", (info.dli_fname != 0 && *info.dli_fname != 0 ? info.dli_fname:"unknow"), iAddr);
	}
	fprintf(fp, "\n");
}

// ͳ�Ƶ�ǰ�����ڴ�����Ķ�ջ��Ϣ
static void StaticStackInfo()
{
	//��ȡ��ǰʱ��
	time_t now = time(0);
	struct tm cur = { 0 };
	localtime_r(&now, &cur);
	char timeNow[64] = { 0 };
	snprintf(timeNow, sizeof(timeNow) - 1, "time_%04d-%02d-%02dT%02d%02d%02d",
		cur.tm_year+1900, cur.tm_mon+1,cur.tm_mday, cur.tm_hour, cur.tm_min, cur.tm_sec);
	//��д���ļ�
	char tmpFile[1024] = { 0 };
	snprintf(tmpFile, sizeof(tmpFile) -1, "%s/%s_%s",g_rptPath, timeNow, g_rptFile);
	FILE* fp = fopen(tmpFile, "wt+");
	if (fp == nullptr)
	{
		PRT("open file:%s failed,err:%s\n",tmpFile, strerror(errno));
		return;
	}
	fprintf(fp, "%s\n", g_rptExePath);
	//����hashtableд���ջ
	for (int i = 0; i < TABLE_SIZE; ++i)
	{
		for (BtInfo* tmp = g_hashTable[i]; tmp != nullptr; tmp = tmp->next)
		{
			if (tmp->count <= 0)
			{
				continue;
			}
			RecordSingleStack(tmp, fp);
		}
	}
	fclose(fp);
}

// �źŴ�����
static void SigHandle(int sigNum)
{
	std::thread thd(StaticStackInfo);
	thd.detach();
}

//  __attribute__((constructor)) -- �ú�����main����֮ǰ��ִ�У���ɳ�ʼ��
__attribute__((constructor)) static void InitAll() 
{
	static int g_init = 0;
	if (g_init == 1)
	{
		return;
	}
	g_init = 1;
	InitMemFunc();
	InitConfig();
	InitBtFunc();
	void(*sign)(int);
	sign = signal(g_rptSignal, SigHandle);
	if (sign == SIG_ERR)
	{
		PRT("signal failed, signNum:%d\n", g_rptSignal);
	}
	g_bInitFlag = true;
}

//����ջ֡��ַ��ȡkeyֵ
static size_t GetHashValue(void** stack, int size)
{
	size_t seed = 0;
	for (int i = 0; i < size; ++i)
	{
		seed ^= std::hash<void*>()(stack[i]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}
	return seed;
}
// �ж��Ƿ���ͬһ��ջ
static bool IsEqual(const BtInfo* ptr1, const BtInfo* ptr2)
{
	if (ptr1->hash != ptr2->hash)
	{
		return false;
	}
	if (ptr1->depth != ptr2->depth)
	{
		return false;
	}
	for (int i = 0; i < ptr1->depth; ++i)
	{
		if (ptr1->bt[i] != ptr2->bt[i])
		{
			return false;
		}
	}
	return true;
}

// �������ڴ��ջ��Ϣ����hashtable��
// ptr -- ��ʵ������ڴ��ַ  == ������չ�ڴ�+�����û��ڴ�
//totalLength: ʵ���ܳ��� 
//extraLength: totalLength - �û�����
static void AddStackInfo(void* ptr, int totalLength, int extraLength)
{
	PtrRep* pRep = (PtrRep*)((char*)ptr + extraLength - sizeof(PtrRep));
	pRep->flag = FLAG_VALUE;
	BtInfo* cur = new BtInfo;
	cur->next = nullptr;
	cur->requestLength = totalLength - extraLength;
	cur->extraLength = extraLength;
	cur->count = 1;
	cur->depth = g_pfnBackTrace(cur->bt, g_maxStackDepth);
	cur->hash = GetHashValue(cur->bt, cur->depth);
	int index = cur->hash % TABLE_SIZE;
	//�����������Ϊ�գ��滻Ϊcur��������ԭ�ȵ�nullptr
	BtInfo* tmp = (BtInfo*)__sync_val_compare_and_swap(&g_hashTable[index],nullptr,cur);
	if (tmp == nullptr)
	{//��һ�β���
		pRep->stack = cur;
		return;
	}
	while (true)
	{	
		if (IsEqual(tmp, cur))
		{//����������һ
			__sync_add_and_fetch(&(tmp->count), 1);
			pRep->stack = tmp;
			delete cur;
			return;
		}
		// tmpָ��������һ��
		tmp = (BtInfo*)__sync_val_compare_and_swap(&tmp->next, nullptr, cur);
		if(tmp == nullptr)
		{//�Ѿ���������β
			pRep->stack = cur;
			return;
		}
	}
	return;
}

//hashtable��������ڴ��ջ��ϢӦ�ü�����һ
//ptr: ��Σ��û�ָ�� ���Σ�mallocʵ�ʵ�ַ
static void DelStackInfo(void* &ptr)
{
	PtrRep* pRep = (PtrRep*)((char*)ptr - sizeof(PtrRep));
	int exterLength = pRep->stack->extraLength;
	__sync_sub_and_fetch(&pRep->stack->count,1);
	ptr = (void*)((char*)ptr - exterLength);
}
void* malloc(size_t bytes)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnMalloc(bytes);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnMalloc(bytes);
	}
	g_tlsKey = 1;
	int extraLength = sizeof(PtrRep);
	bytes += extraLength;
	void* pRet = malloc(bytes);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, bytes, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void free(void* mem)
{
	InitAll();
	if (!g_bInitFlag)
	{
		g_pfnFree(mem);
		return;
	}
	if (mem == nullptr)
	{
		return;
	}
	if (g_tlsKey == 1)
	{
		g_pfnFree(mem);
		return;
	}
	size_t flag = *(size_t*)((char*)mem - sizeof(size_t));
	if (flag != FLAG_VALUE)// �ж��Ƿ�Ϊmla��װ����������ڴ�
	{
		g_pfnFree(mem);
		return;
	}
	g_tlsKey = 1;
	DelStackInfo(mem);
	free(mem);
	g_tlsKey = 0;
}

void* calloc(size_t cnt, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnCalloc == nullptr? nullptr:g_pfnCalloc(cnt, size);
	}
	size_t bytes = cnt*size; // ��ֹoverflow
	if (size != 0 && bytes / size != cnt)
	{
		return 0;
	}
	if (g_tlsKey == 1)
	{
		return g_pfnCalloc(cnt, size);
	}
	g_tlsKey = 1;
	int extraLength = sizeof(PtrRep);
	bytes += extraLength;
	void* pRet = calloc(bytes, 1);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, bytes, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void* realloc(void* ptr, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnRealloc(ptr, size);
	}
	if (size == 0 && ptr != nullptr)
	{
		free(ptr);
		return nullptr;
	}
	if (ptr == nullptr)
	{
		return malloc(size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnRealloc(ptr, size);
	}
	g_tlsKey = 1;
	PtrRep* pRep = (PtrRep*)((char*)ptr - sizeof(PtrRep));
	BtInfo* stack = pRep->stack;
	void* newPtr = (void*)((char*)ptr - pRep->stack->extraLength);
	int extraLength = sizeof(PtrRep);
	size += extraLength;
	void* pRet = realloc(newPtr, size);
	if (pRet != nullptr)
	{
		__sync_sub_and_fetch(&stack->count, 1);
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}  

void* memalign(size_t alignment, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnMemalign(alignment, size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnMemalign(alignment, size);
	}
	g_tlsKey = 1;
	int extraLength = alignment;
	if (alignment < sizeof(PtrRep))
	{
		extraLength = (sizeof(PtrRep) / alignment + 1) * sizeof(PtrRep);
	}
	size += extraLength;
	void* pRet = memalign(alignment, size);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void* valloc(size_t size)
{
	return memalign(g_pageSize, size);
}

void* pvalloc(size_t size)
{
	size_t roundedBytes = (size + g_pageSize - 1)&~(g_pageSize - 1);
	return memalign(g_pageSize, roundedBytes);
}

int posix_memalign(void** ptr, size_t alignment, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnPosixMemalign(ptr, alignment, size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnPosixMemalign(ptr, alignment, size);
	}
	g_tlsKey = 1;
	int extraLength = alignment;
	if (alignment < sizeof(PtrRep))
	{
		extraLength = (sizeof(PtrRep) / alignment + 1) * sizeof(PtrRep);
	}
	size += extraLength;
	void* pRet = nullptr;
	int iRet = posix_memalign(&pRet, alignment, size);
	if (iRet == 0 && pRet != nullptr)
	{
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	*ptr = pRet;
	return iRet;
}

//c11
void* aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}
















