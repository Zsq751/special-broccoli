/**
 * ============================================================================
 * 基于霍夫曼编码的文件压缩软件 - 支持目录递归，保留目录结构
 * ============================================================================
 * 
 * 功能说明：
 *   1. 压缩单个或多个文件，若输入目录则自动递归包含其下所有文件
 *   2. 解压 .huf 压缩包，可指定输出目录
 *   3. 显示原始大小、压缩后大小、压缩率和耗时
 *   4. 自动兼容缺少 .txt 后缀的文件（例如传入 C3-Art0001 也能找到 C3-Art0001.txt）
 *   5. 压缩目录时会保留顶层目录名，解压后恢复完整目录结构
 * 
 * 使用方法：
 *   压缩：huffman.exe -c 压缩包.huf 文件1 目录1 文件2 ... [-v]
 *   解压：huffman.exe -x 压缩包.huf [输出目录] [-v]
 *   选项：-v  显示详细处理过程（可放在任意位置）
 * 
 * 编译命令：
 *   gcc -o huffman.exe huffman.c -O2 -Wall
 * 
 * ============================================================================
 */

#include <stdio.h>      // 标准输入输出（文件读写）
#include <stdlib.h>     // 动态内存分配（malloc/free）和程序退出
#include <string.h>     // 字符串操作（strcpy, strcat, memcpy等）
#include <windows.h>    // Windows API（目录遍历、创建目录、文件属性查询）
#include <time.h>       // 计时函数 clock()

// ----------------------------- 常量定义 ------------------------------------
#define MAGIC "HUFF"            // 压缩包文件头标识（4字节），用于识别格式
#define VERSION 1               // 格式版本号，便于未来扩展
#define FREQ_SIZE 256           // 字节值的范围是 0~255，共256种
#define MAX_PATH 260            // Windows 环境下最大路径长度

// ----------------------------- 数据结构定义 --------------------------------
// 用于存储文件信息的结构体（完整路径 + 归档相对路径）
struct FileEntry {
	char full_path[MAX_PATH];   // 磁盘上的完整路径（用于读取文件）
	char arc_path[MAX_PATH];    // 压缩包内存储的相对路径（用于解压时重建目录）
};

// 霍夫曼树节点
struct HuffNode {
	unsigned char ch;               // 字符（仅叶子节点有效，表示原始字节）
	unsigned long long freq;        // 该字符在数据中出现的次数（频率）
	struct HuffNode *left;          // 左孩子指针（编码时代表0）
	struct HuffNode *right;         // 右孩子指针（编码时代表1）
};

// 霍夫曼编码表：为每个字符存储对应的二进制码和位长
struct HuffCode {
	unsigned int code;      // 二进制编码（最高支持24位，足够存储256个变长码）
	unsigned char len;      // 编码长度（位数）
};

// ----------------------------- 全局变量 ------------------------------------
int verbose = 0;    // 详细输出标志，当命令行带有 -v 时设置为1，用于显示中间信息

// ----------------------------- 内存分配包装函数 ----------------------------
/**
 * 安全的内存分配函数，失败时打印错误并退出程序
 */
void* my_malloc(size_t size) {
	void *p = malloc(size);             // 调用标准库分配内存
	if (p == NULL) {                    // 检查是否分配失败
		printf("错误：内存分配失败！程序退出。\n");
		exit(1);                        // 直接退出程序
	}
	return p;                           // 返回分配的内存指针
}

// ----------------------------- 霍夫曼树操作函数 ----------------------------
/**
 * 递归释放整棵霍夫曼树的所有节点
 */
void free_tree(struct HuffNode *root) {
	if (root == NULL) return;           // 空节点直接返回
	free_tree(root->left);              // 递归释放左子树
	free_tree(root->right);             // 递归释放右子树
	free(root);                         // 释放当前节点
}

/**
 * 创建一个新的霍夫曼树节点
 * @param ch   字符值（仅叶子节点有意义）
 * @param freq 频率（权重）
 * @return 新节点指针
 */
struct HuffNode* create_node(unsigned char ch, unsigned long long freq) {
	struct HuffNode *node = (struct HuffNode*)my_malloc(sizeof(struct HuffNode)); // 分配节点内存
	node->ch = ch;                  // 设置字符
	node->freq = freq;              // 设置频率
	node->left = NULL;              // 左孩子初始为空
	node->right = NULL;             // 右孩子初始为空
	return node;                    // 返回新节点
}

/**
 * 根据频率表构建霍夫曼树（使用简单数组模拟最小堆，每次合并最小两个节点）
 * @param freq 长度为256的无符号整数数组，存储每个字节的频率
 * @return 霍夫曼树的根节点指针
 */
struct HuffNode* build_tree(unsigned int freq[FREQ_SIZE]) {
	struct HuffNode* nodes[FREQ_SIZE];  // 存储节点的临时数组
	int cnt = 0;                        // 当前节点数量
	for (int i = 0; i < FREQ_SIZE; i++) {
		if (freq[i] > 0) {              // 只处理出现过的字符
			nodes[cnt++] = create_node((unsigned char)i, freq[i]); // 创建叶子节点
		}
	}
	if (cnt == 0) {                     // 如果没有任何字符（空文件）
		nodes[cnt++] = create_node(0, 1); // 创建一个虚拟节点，避免空树
	}
	while (cnt > 1) {                   // 反复合并，直到只剩一个根节点
		// 找出频率最小的两个节点的下标（min1 最小，min2 次小）
		int min1 = 0, min2 = 1;
		if (nodes[min1]->freq > nodes[min2]->freq) {
			int t = min1; min1 = min2; min2 = t; // 确保 min1 是最小的
		}
		for (int i = 2; i < cnt; i++) {
			if (nodes[i]->freq < nodes[min1]->freq) {
				min2 = min1;
				min1 = i;
			} else if (nodes[i]->freq < nodes[min2]->freq) {
				min2 = i;
			}
		}
		// 创建新节点，左孩子为最小节点，右孩子为次小节点
		struct HuffNode *parent = create_node(0, nodes[min1]->freq + nodes[min2]->freq);
		parent->left = nodes[min1];     // 左孩子指向最小节点
		parent->right = nodes[min2];    // 右孩子指向次小节点
		// 将新节点放在 min1 位置，把最后一个节点移到 min2 位置，并减少计数
		nodes[min1] = parent;
		nodes[min2] = nodes[cnt - 1];
		cnt--;
	}
	return nodes[0];   // 返回根节点
}

/**
 * 递归遍历霍夫曼树，为每个叶子节点生成编码（前缀码）
 * @param node   当前节点
 * @param code   从根到当前节点的二进制路径值（左0右1）
 * @param len    路径长度（位数）
 * @param codes  输出数组，用于存储每个字符的编码
 */
void make_codes(struct HuffNode *node, unsigned int code, unsigned char len, struct HuffCode codes[FREQ_SIZE]) {
	if (node->left == NULL && node->right == NULL) { // 叶子节点
		codes[node->ch].code = code;    // 存储编码值
		codes[node->ch].len = len;      // 存储编码长度
		return;
	}
	// 向左走：编码末尾追加0
	if (node->left)
		make_codes(node->left, code << 1, len + 1, codes);
	// 向右走：编码末尾追加1
	if (node->right)
		make_codes(node->right, (code << 1) | 1, len + 1, codes);
}

// ----------------------------- 压缩和解压核心算法 --------------------------
/**
 * 霍夫曼压缩一块内存数据
 * @param data     原始数据指针
 * @param data_len 原始数据长度（字节数）
 * @param out_len  输出参数：压缩后的数据长度
 * @return 指向压缩数据的指针（需要调用者 free）
 */
unsigned char* huffman_compress(const unsigned char *data, unsigned long long data_len, unsigned long long *out_len) {
	if (data_len == 0) {    // 空文件特殊处理
		*out_len = sizeof(unsigned long long) + FREQ_SIZE * sizeof(unsigned int); // 头部大小
		unsigned char *out = (unsigned char*)my_malloc(*out_len);
		memset(out, 0, *out_len);               // 全部填0
		*(unsigned long long*)out = 0;          // 原始大小置0
		return out;
	}
	unsigned int freq[FREQ_SIZE] = {0};         // 频率表初始化为0
	for (unsigned long long i = 0; i < data_len; i++) {
		freq[data[i]]++;                        // 统计每个字节出现次数
	}
	struct HuffNode *root = build_tree(freq);   // 构建霍夫曼树
	struct HuffCode codes[FREQ_SIZE] = {{0,0}}; // 编码表
	make_codes(root, 0, 0, codes);              // 生成编码
	free_tree(root);                            // 树已用完，释放内存
	
	// 将原始数据按编码表转换为比特流
	unsigned char *bit_buffer = (unsigned char*)my_malloc(data_len * 2); // 临时缓冲区（大小足够）
	unsigned long long bit_pos = 0;             // 当前已写入的字节数
	unsigned char buf = 0;                      // 当前字节的缓冲区
	int bits = 0;                               // 当前缓冲区中已填充的位数
	for (unsigned long long i = 0; i < data_len; i++) {
		unsigned char ch = data[i];
		unsigned int code = codes[ch].code;
		unsigned char len = codes[ch].len;
		// 将编码按位写入（从高位到低位，保证解码顺序一致）
		for (int j = len - 1; j >= 0; j--) {
			int bit = (code >> j) & 1;          // 取出当前位
			buf = (buf << 1) | bit;             // 添加到缓冲区
			bits++;
			if (bits == 8) {                    // 凑满一个字节
				bit_buffer[bit_pos++] = buf;
				buf = 0;
				bits = 0;
			}
		}
	}
	if (bits > 0) {                             // 最后不足8位，左移补0对齐到字节
		buf = buf << (8 - bits);
		bit_buffer[bit_pos++] = buf;
	}
	// 构造最终压缩块：原始大小(8字节) + 频率表(1024字节) + 编码比特流
	*out_len = sizeof(unsigned long long) + FREQ_SIZE * sizeof(unsigned int) + bit_pos;
	unsigned char *out = (unsigned char*)my_malloc(*out_len);
	unsigned char *ptr = out;
	memcpy(ptr, &data_len, sizeof(unsigned long long));        // 写入原始长度
	ptr += sizeof(unsigned long long);
	memcpy(ptr, freq, FREQ_SIZE * sizeof(unsigned int));       // 写入频率表
	ptr += FREQ_SIZE * sizeof(unsigned int);
	memcpy(ptr, bit_buffer, bit_pos);                          // 写入编码流
	free(bit_buffer);
	return out;
}

/**
 * 霍夫曼解压一块压缩数据
 * @param comp_data 压缩数据指针
 * @param comp_len  压缩数据长度
 * @param out_len   输出参数：原始数据长度
 * @return 原始数据指针（需要调用者 free），失败返回 NULL
 */
unsigned char* huffman_decompress(const unsigned char *comp_data, unsigned long long comp_len, unsigned long long *out_len) {
	if (comp_len < sizeof(unsigned long long) + FREQ_SIZE * sizeof(unsigned int)) {
		printf("错误：压缩数据不完整。\n");
		return NULL;
	}
	const unsigned char *ptr = comp_data;
	unsigned long long orig_len;
	memcpy(&orig_len, ptr, sizeof(unsigned long long)); // 读取原始长度
	ptr += sizeof(unsigned long long);
	if (orig_len == 0) {                                // 空文件
		*out_len = 0;
		unsigned char *empty = (unsigned char*)malloc(1);
		return empty;
	}
	unsigned int freq[FREQ_SIZE];
	memcpy(freq, ptr, FREQ_SIZE * sizeof(unsigned int)); // 读取频率表
	ptr += FREQ_SIZE * sizeof(unsigned int);
	unsigned long long encoded_len = comp_len - (sizeof(unsigned long long) + FREQ_SIZE * sizeof(unsigned int));
	struct HuffNode *root = build_tree(freq);           // 重建霍夫曼树
	unsigned char *out = (unsigned char*)my_malloc(orig_len);
	unsigned long long out_pos = 0;
	unsigned long long bit_pos = 0;                     // 当前读取的字节位置
	unsigned char bit_buf = 0;                          // 当前字节
	int bit_cnt = 0;                                    // 当前字节中剩余未读的位数
	struct HuffNode *cur = root;                        // 当前树节点
	while (out_pos < orig_len) {
		if (bit_cnt == 0) {                             // 当前字节已读完，取下一个
			if (bit_pos >= encoded_len) break;          // 数据不足，异常
			bit_buf = ptr[bit_pos++];
			bit_cnt = 8;
		}
		bit_cnt--;
		int bit = (bit_buf >> bit_cnt) & 1;             // 取出一位
		if (bit == 0)
			cur = cur->left;                            // 向左走
		else
			cur = cur->right;                           // 向右走
		if (cur->left == NULL && cur->right == NULL) {  // 到达叶子节点
			out[out_pos++] = cur->ch;                   // 输出字符
			cur = root;                                 // 回到根节点继续
		}
	}
	free_tree(root);
	if (out_pos != orig_len) {                          // 长度不匹配，解压失败
		free(out);
		printf("错误：解压后长度不匹配（期望 %llu，实际 %llu）\n", orig_len, out_pos);
		return NULL;
	}
	*out_len = orig_len;
	return out;
}

// ----------------------------- 文件读写辅助函数 ----------------------------
/**
 * 读取整个文件内容到内存
 * @param filename 文件名
 * @param size     输出文件大小（字节数）
 * @return 文件数据指针（需调用者 free），失败返回 NULL
 */
unsigned char* read_whole_file(const char *filename, unsigned long long *size) {
	FILE *fp = fopen(filename, "rb");   // 以二进制只读方式打开
	if (fp == NULL) return NULL;
	fseek(fp, 0, SEEK_END);             // 定位到文件末尾
	*size = ftell(fp);                  // 获取文件大小
	fseek(fp, 0, SEEK_SET);             // 回到文件开头
	unsigned char *data = (unsigned char*)my_malloc(*size);
	fread(data, 1, *size, fp);          // 读取全部内容
	fclose(fp);
	return data;
}

/**
 * 将数据写入文件，并自动创建所需的目录路径
 * @param filename 输出文件名（可以包含路径）
 * @param data     数据指针
 * @param size     数据大小
 * @return 1成功，0失败
 */
int write_whole_file(const char *filename, const unsigned char *data, unsigned long long size) {
	char dir[MAX_PATH];
	strcpy(dir, filename);
	char *p = strrchr(dir, '\\');       // 查找最后一个反斜杠
	if (p != NULL) {
		*p = '\0';                      // 截断得到目录部分
		char *q = dir;
		while ((q = strchr(q, '\\')) != NULL) {
			*q = '\0';
			CreateDirectoryA(dir, NULL); // 逐级创建目录
			*q = '\\';
			q++;
		}
		CreateDirectoryA(dir, NULL);    // 创建最后一级目录
	}
	FILE *fp = fopen(filename, "wb");
	if (fp == NULL) return 0;
	fwrite(data, 1, size, fp);
	fclose(fp);
	return 1;
}

// ----------------------------- 目录递归函数（核心新增功能）-----------------
/**
 * 递归遍历目录，将文件信息添加到数组
 * @param dir_path  当前目录的完整路径
 * @param list      动态数组指针
 * @param cnt       当前数量
 * @param cap       容量
 * @param base_dir  基准父目录（用于计算相对路径，应包含顶层目录名）
 */
void add_directory_files(const char *dir_path, struct FileEntry **list, int *cnt, int *cap, const char *base_dir) {
	char search_path[MAX_PATH];
	snprintf(search_path, sizeof(search_path), "%s\\*", dir_path); // 搜索模式：目录\*
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(search_path, &fd); // 开始遍历目录
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
			continue;                           // 跳过当前目录和上级目录
		char full_path[MAX_PATH];
		snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			// 如果是子目录，递归调用
			add_directory_files(full_path, list, cnt, cap, base_dir);
		} else {
			// 计算相对路径：去掉 base_dir 前缀（base_dir 是父目录，因此结果会包含顶层目录名）
			char rel_path[MAX_PATH];
			const char *p = full_path + strlen(base_dir);
			if (*p == '\\') p++;                // 跳过开头的反斜杠
			strcpy(rel_path, p);                // 得到相对路径，如 "C11-Space\file1"
			// 动态扩容
			if (*cnt >= *cap) {
				*cap *= 2;
				*list = (struct FileEntry*)realloc(*list, (*cap) * sizeof(struct FileEntry));
				if (*list == NULL) { printf("内存分配失败\n"); exit(1); }
			}
			strcpy((*list)[*cnt].full_path, full_path); // 保存完整路径
			strcpy((*list)[*cnt].arc_path, rel_path);   // 保存归档相对路径
			(*cnt)++;
			if (verbose) printf("  添加文件: %s (完整路径: %s)\n", rel_path, full_path);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
}

/**
 * 收集命令行输入的文件和目录（支持自动补全 .txt 后缀）
 * @param inputs      命令行参数数组（从 -c 之后开始）
 * @param input_count 参数个数
 * @param out_entries 输出：文件条目数组（需要调用者释放）
 * @return 文件数量
 */
int collect_files(char **inputs, int input_count, struct FileEntry **out_entries) {
	int cap = 64;                                       // 初始容量
	int cnt = 0;
	struct FileEntry *entries = (struct FileEntry*)my_malloc(cap * sizeof(struct FileEntry));
	for (int i = 0; i < input_count; i++) {
		char *input = inputs[i];
		char actual_path[MAX_PATH];
		strcpy(actual_path, input);                     // 复制输入路径，避免修改原参数
		DWORD attrs = GetFileAttributesA(actual_path); // 获取文件属性
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			// 自动补全 .txt 后缀
			char with_txt[MAX_PATH];
			snprintf(with_txt, sizeof(with_txt), "%s.txt", actual_path);
			attrs = GetFileAttributesA(with_txt);
			if (attrs != INVALID_FILE_ATTRIBUTES) {
				strcpy(actual_path, with_txt);          // 使用带 .txt 的路径
				if (verbose) printf("  路径自动补全 .txt: %s -> %s\n", input, actual_path);
			} else {
				printf("警告：文件或目录不存在 - %s，已跳过\n", input);
				continue;
			}
		}
		if (attrs & FILE_ATTRIBUTE_DIRECTORY) {         // 是目录
			if (verbose) printf("扫描目录: %s\n", actual_path);
			char parent_dir[MAX_PATH];
			strcpy(parent_dir, actual_path);
			char *last_backslash = strrchr(parent_dir, '\\');
			if (last_backslash != NULL) {
				*last_backslash = '\0';                 // 截断得到父目录，使相对路径包含当前目录名
			} else {
				parent_dir[0] = '\0';                   // 没有反斜杠，设为空字符串
			}
			add_directory_files(actual_path, &entries, &cnt, &cap, parent_dir);
		} else {                                         // 是普通文件
			if (cnt >= cap) {
				cap *= 2;
				entries = (struct FileEntry*)realloc(entries, cap * sizeof(struct FileEntry));
			}
			strcpy(entries[cnt].full_path, actual_path); // 完整路径
			strcpy(entries[cnt].arc_path, actual_path);  // 归档路径与完整路径相同（无额外目录）
			cnt++;
			if (verbose) printf("  添加文件: %s\n", actual_path);
		}
	}
	*out_entries = entries;
	return cnt;
}

// ----------------------------- 压缩/解压主逻辑 ----------------------------
/**
 * 将多个文件压缩成一个归档文件（.huf）
 * @param arch_name 压缩包文件名
 * @param entries   文件条目数组
 * @param file_cnt  文件个数
 * @return 1成功，0失败
 */
int compress_files(const char *arch_name, struct FileEntry *entries, int file_cnt) {
	FILE *arch = fopen(arch_name, "wb");                // 创建压缩包
	if (!arch) {
		printf("无法创建压缩包 %s\n", arch_name);
		return 0;
	}
	fwrite(MAGIC, 4, 1, arch);                          // 写入魔数 "HUFF"
	unsigned char ver = VERSION;
	fwrite(&ver, 1, 1, arch);                           // 写入版本号
	unsigned char flags = 0;
	fwrite(&flags, 1, 1, arch);                         // 保留标志
	unsigned int count = file_cnt;
	fwrite(&count, 4, 1, arch);                         // 写入文件数量
	// 临时存储每个文件的压缩结果（因为需要先压缩完才能写元数据）
	struct {
		char arc_path[MAX_PATH];
		unsigned long long orig_size;
		unsigned long long comp_size;
		unsigned char *comp_data;
	} *temp = (void*)my_malloc(file_cnt * sizeof(*temp));
	unsigned long long total_orig = 0, total_comp = 0;
	for (int i = 0; i < file_cnt; i++) {
		if (verbose) printf("  正在压缩: %s\n", entries[i].full_path);
		unsigned long long orig_size;
		unsigned char *orig_data = read_whole_file(entries[i].full_path, &orig_size);
		if (orig_data == NULL) {                        // 读取失败，尝试补全 .txt 后缀
			char with_txt[MAX_PATH];
			snprintf(with_txt, sizeof(with_txt), "%s.txt", entries[i].full_path);
			if (verbose) printf("  尝试补全 .txt: %s\n", with_txt);
			orig_data = read_whole_file(with_txt, &orig_size);
			if (orig_data == NULL) {
				printf("无法读取文件 %s（尝试 %s 也失败），跳过\n", entries[i].full_path, with_txt);
				continue;
			} else if (verbose) {
				printf("  文件 %s 以 %s 形式成功读取\n", entries[i].full_path, with_txt);
			}
		}
		unsigned long long comp_size;
		unsigned char *comp_data = huffman_compress(orig_data, orig_size, &comp_size);
		free(orig_data);
		strcpy(temp[i].arc_path, entries[i].arc_path);
		temp[i].orig_size = orig_size;
		temp[i].comp_size = comp_size;
		temp[i].comp_data = comp_data;
		total_orig += orig_size;
		total_comp += comp_size;
	}
	// 写入每个文件的元数据和压缩块
	for (int i = 0; i < file_cnt; i++) {
		if (temp[i].comp_data == NULL) continue;       // 跳过读取失败的文件
		unsigned short name_len = (unsigned short)strlen(temp[i].arc_path);
		fwrite(&name_len, 2, 1, arch);                  // 文件名长度
		fwrite(temp[i].arc_path, 1, name_len, arch);    // 文件名
		fwrite(&temp[i].orig_size, 8, 1, arch);         // 原始大小
		fwrite(&temp[i].comp_size, 8, 1, arch);         // 压缩后大小
		fwrite(temp[i].comp_data, 1, temp[i].comp_size, arch); // 压缩数据块
		free(temp[i].comp_data);
	}
	free(temp);
	fclose(arch);
	printf("\n文件数量：%d\n", file_cnt);
	printf("原始总大小：%llu 字节\n", total_orig);
	printf("压缩后大小：%llu 字节\n", total_comp);
	if (total_orig > 0) {
		double ratio = (double)total_comp / total_orig * 100.0;
		printf("压缩率：%.2f%%\n", ratio);
	}
	return 1;
}

/**
 * 解压归档文件
 * @param arch_name 压缩包文件名
 * @param out_dir   输出目录（若为 NULL 或空字符串，则解压到当前目录）
 * @return 1成功，0失败
 */
int extract_archive(const char *arch_name, const char *out_dir) {
	FILE *arch = fopen(arch_name, "rb");
	if (!arch) {
		printf("无法打开压缩包 %s\n", arch_name);
		return 0;
	}
	char magic[4];
	fread(magic, 4, 1, arch);                           // 读取魔数
	if (memcmp(magic, MAGIC, 4) != 0) {                 // 验证格式
		printf("错误：不是有效的霍夫曼压缩文件！\n");
		fclose(arch);
		return 0;
	}
	unsigned char ver, flags;
	fread(&ver, 1, 1, arch);                            // 版本号（暂未使用）
	fread(&flags, 1, 1, arch);                          // 标志（保留）
	unsigned int file_cnt;
	fread(&file_cnt, 4, 1, arch);                       // 文件数量
	for (unsigned int i = 0; i < file_cnt; i++) {
		unsigned short name_len;
		fread(&name_len, 2, 1, arch);                   // 文件名长度
		char *filename = (char*)my_malloc(name_len + 1);
		fread(filename, 1, name_len, arch);             // 读取文件名
		filename[name_len] = '\0';
		unsigned long long orig_sz, comp_sz;
		fread(&orig_sz, 8, 1, arch);                    // 原始大小
		fread(&comp_sz, 8, 1, arch);                    // 压缩大小
		unsigned char *comp_data = (unsigned char*)my_malloc(comp_sz);
		fread(comp_data, 1, comp_sz, arch);             // 读取压缩数据块
		unsigned long long out_sz;
		unsigned char *orig_data = huffman_decompress(comp_data, comp_sz, &out_sz);
		free(comp_data);
		if (orig_data && out_sz == orig_sz) {
			char out_path[MAX_PATH];
			if (out_dir != NULL && out_dir[0] != '\0') {
				snprintf(out_path, sizeof(out_path), "%s\\%s", out_dir, filename);
			} else {
				strcpy(out_path, filename);
			}
			if (verbose) printf("  解压: %s -> %s\n", filename, out_path);
			if (!write_whole_file(out_path, orig_data, out_sz)) {
				printf("写入文件失败 %s\n", out_path);
			}
			free(orig_data);
		} else {
			printf("解压失败 %s\n", filename);
		}
		free(filename);
	}
	fclose(arch);
	return 1;
}

// ----------------------------- 命令行帮助和主函数 --------------------------
/**
 * 显示帮助信息
 */
void help(const char *prog) {
	printf("用法:\n");
	printf("  压缩: %s -c 包名.huf 文件1 目录1 文件2 ... [-v]\n", prog);
	printf("  解压: %s -x 包名.huf [输出目录] [-v]\n", prog);
	printf("  选项: -v 显示详细信息（可放在任意位置）\n");
	printf("说明: 输入目录时会递归包含该目录下所有文件，并保留目录结构。\n");
	printf("改进: 自动兼容缺少 .txt 后缀的文件（例如 C3-Art0001 会尝试 C3-Art0001.txt）\n");
}

/**
 * 主函数：解析命令行，调用压缩或解压功能
 */
int main(int argc, char **argv) {
	if (argc < 3) {                                     // 参数不足
		help(argv[0]);
		return 1;
	}
	int verbose_flag = 0;
	int mode_idx = -1;                                  // -c 或 -x 的索引
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			verbose_flag = 1;
		} else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-x") == 0) {
			mode_idx = i;
		}
	}
	verbose = verbose_flag;
	if (mode_idx == -1) {                               // 未指定模式
		printf("错误：必须指定 -c（压缩）或 -x（解压）模式。\n");
		return 1;
	}
	int compress = 0, extract = 0;
	if (strcmp(argv[mode_idx], "-c") == 0) compress = 1;
	else extract = 1;
	char *archive = NULL;
	int arch_idx = mode_idx + 1;
	while (arch_idx < argc && strcmp(argv[arch_idx], "-v") == 0) arch_idx++;
	if (arch_idx >= argc) {                             // 未指定压缩包名
		printf("错误：未指定压缩包文件名。\n");
		return 1;
	}
	archive = argv[arch_idx];
	int input_count = 0;
	char **input_list = NULL;
	for (int i = arch_idx + 1; i < argc; i++) {         // 收集输入文件/目录
		if (strcmp(argv[i], "-v") == 0) continue;
		input_list = (char**)realloc(input_list, (input_count + 1) * sizeof(char*));
		input_list[input_count++] = argv[i];
	}
	char *output_dir = NULL;
	if (extract && input_count > 0) {                   // 解压模式下可能的输出目录
		if (input_list[0][0] != '-') {
			output_dir = input_list[0];
			if (input_count > 1) {
				printf("警告：解压模式只接受一个可选的输出目录，多余参数将被忽略。\n");
			}
		} else {
			if (input_count > 0) {
				printf("警告：解压模式不需要指定输入文件，多余参数将被忽略。\n");
			}
		}
	}
	if (compress && input_count == 0) {                 // 压缩模式没有输入
		printf("错误：压缩模式需要至少一个输入文件或目录。\n");
		free(input_list);
		return 1;
	}
	if (compress) {
		struct FileEntry *entries = NULL;
		int file_cnt = collect_files(input_list, input_count, &entries);
		free(input_list);
		if (file_cnt == 0) {
			printf("错误：没有找到任何有效文件。\n");
			return 1;
		}
		if (verbose) printf("共找到 %d 个文件\n", file_cnt);
		clock_t start = clock();
		int ok = compress_files(archive, entries, file_cnt);
		clock_t end = clock();
		if (ok) {
			printf("耗时：%.3f 秒\n", (double)(end - start) / CLOCKS_PER_SEC);
		} else {
			printf("压缩失败！\n");
		}
		free(entries);
		return ok ? 0 : 1;
	}
	if (extract) {
		free(input_list);
		clock_t start = clock();
		int ok = extract_archive(archive, output_dir);
		clock_t end = clock();
		if (ok) {
			printf("解压完成，耗时：%.3f 秒\n", (double)(end - start) / CLOCKS_PER_SEC);
		} else {
			printf("解压失败！\n");
		}
		return ok ? 0 : 1;
	}
	free(input_list);
	return 0;
}
