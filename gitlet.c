#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <direct.h>
#include <windows.h>

/* ================= 配置与常量 ================= */
#define MAX_PATH_LEN 260
#define MAX_MSG_LEN 1024
#define MAX_FILES_IN_COMMIT 200
#define HASH_LEN 9 // 8 chars + null terminator

const char* GITLET_DIR = ".gitlet";
const char* COMMITS_DIR = ".gitlet/commits";
const char* BLOBS_DIR = ".gitlet/blobs";
const char* BRANCHES_DIR = ".gitlet/branches";
const char* STAGING_DIR = ".gitlet/staging";
const char* STAGING_FILE = ".gitlet/staging/staging.txt";
const char* HEAD_FILE = ".gitlet/branches.txt";

/* ================= 数据结构 ================= */
typedef struct {
    char path[MAX_PATH_LEN];
    char blob_hash[HASH_LEN];
} FileEntry;

typedef struct {
    char hash[HASH_LEN];
    char parent[HASH_LEN];
    char merge_parent[HASH_LEN];
    char timestamp[64];
    char message[MAX_MSG_LEN];
    int file_count;
    FileEntry files[MAX_FILES_IN_COMMIT];
} Commit;

/* ================= 工具函数 ================= */

// DJB2 哈希算法 
void compute_hash(const char* str, char* output) {
    unsigned long hash = 5381;
    int c;
    const char* s = str;
    while ((c = *s++)) {
        hash = ((hash << 5) + hash) + c; 
    }
    sprintf(output, "%08lx", hash);
}

// 检查文件是否存在
int file_exists(const char* path) {
    return _access(path, 0) == 0;
}

// 获取当前时间戳
void get_timestamp(char* buffer) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, 64, "%Y-%m-%d %H:%M:%S", t);
}

// 读取文件全部内容
char* read_file_content(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = (char*)malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, f);
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

// 写入文件内容
void write_file_content(const char* path, const char* content, int is_binary, long length) {
    FILE* f = fopen(path, is_binary ? "wb" : "w");
    if (f) {
        fwrite(content, 1, length, f);
        fclose(f);
    }
}

// 递归创建目录 (Windows)
void ensure_dir(const char* path) {
    char tmp[MAX_PATH_LEN];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            _mkdir(tmp);
            *p = '/';
        }
    }
    _mkdir(tmp);
}

/* ================= 核心逻辑：序列化与对象管理 ================= */

// 保存 Commit 对象到文件
void save_commit(Commit* c) {
    char filepath[MAX_PATH_LEN];
    
    // 如果没有哈希，先计算
    if (strlen(c->hash) == 0) {
        // 哈希基于所有字段计算，这里简化：基于消息+时间+父哈希
        char raw_data[4096];
        sprintf(raw_data, "%s%s%s", c->message, c->timestamp, c->parent);
        compute_hash(raw_data, c->hash);
    }
    
    sprintf(filepath, "%s/%s", COMMITS_DIR, c->hash);
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "hash:%s\n", c->hash);
    fprintf(f, "parent:%s\n", c->parent);
    fprintf(f, "merge_parent:%s\n", c->merge_parent);
    fprintf(f, "timestamp:%s\n", c->timestamp);
    fprintf(f, "message:%s\n", c->message);
    for (int i = 0; i < c->file_count; i++) {
        fprintf(f, "file:%s %s\n", c->files[i].path, c->files[i].blob_hash);
    }
    fclose(f);
}

// 从文件加载 Commit
int load_commit(const char* hash, Commit* c) {
    char filepath[MAX_PATH_LEN];
    sprintf(filepath, "%s/%s", COMMITS_DIR, hash);
    FILE* f = fopen(filepath, "r");
    if (!f) return 0;

    char line[MAX_MSG_LEN];
    c->file_count = 0;
    c->merge_parent[0] = '\0'; // init

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0; // 去除换行
        if (strncmp(line, "hash:", 5) == 0) strcpy(c->hash, line + 5);
        else if (strncmp(line, "parent:", 7) == 0) strcpy(c->parent, line + 7);
        else if (strncmp(line, "merge_parent:", 13) == 0) strcpy(c->merge_parent, line + 13);
        else if (strncmp(line, "timestamp:", 10) == 0) strcpy(c->timestamp, line + 10);
        else if (strncmp(line, "message:", 8) == 0) strcpy(c->message, line + 8);
        else if (strncmp(line, "file:", 5) == 0) {
            char* ptr = line + 5;
            char* space = strchr(ptr, ' ');
            if (space) {
                *space = 0;
                strcpy(c->files[c->file_count].path, ptr);
                strcpy(c->files[c->file_count].blob_hash, space + 1);
                c->file_count++;
            }
        }
    }
    fclose(f);
    return 1;
}

// 获取 HEAD 指向的 Commit Hash
void get_head_commit_hash(char* hash_out) {
    char branch_name[MAX_PATH_LEN];
    FILE* f = fopen(HEAD_FILE, "r");
    if (!f) { hash_out[0] = 0; return; }
    fgets(branch_name, sizeof(branch_name), f);
    fclose(f);
    branch_name[strcspn(branch_name, "\n")] = 0;

    char branch_path[MAX_PATH_LEN];
    sprintf(branch_path, "%s/%s", BRANCHES_DIR, branch_name);
    f = fopen(branch_path, "r");
    if (f) {
        fgets(hash_out, HASH_LEN + 1, f);
        fclose(f);
    } else {
        // Detached head case (implied complexity, skipping for simplicity, assuming on branch)
        hash_out[0] = 0;
    }
}

// 更新 HEAD 分支指向新的 Commit
void update_head_branch(const char* new_hash) {
    char branch_name[MAX_PATH_LEN];
    FILE* f = fopen(HEAD_FILE, "r");
    if (!f) return;
    fgets(branch_name, sizeof(branch_name), f);
    fclose(f);
    branch_name[strcspn(branch_name, "\n")] = 0;

    char branch_path[MAX_PATH_LEN];
    sprintf(branch_path, "%s/%s", BRANCHES_DIR, branch_name);
    write_file_content(branch_path, new_hash, 0, strlen(new_hash));
}

/* ================= 命令实现 ================= */

void cmd_init() { // 
    if (file_exists(GITLET_DIR)) {
        printf("A Gitlet version-control system already exists in the current directory.\n");
        return;
    }
    _mkdir(GITLET_DIR);
    _mkdir(COMMITS_DIR);
    _mkdir(BLOBS_DIR);
    _mkdir(BRANCHES_DIR);
    ensure_dir(STAGING_DIR);

    // Initial Commit
    Commit c;
    memset(&c, 0, sizeof(Commit));
    get_timestamp(c.timestamp);
    strcpy(c.message, "initial commit");
    strcpy(c.hash, "00000000"); // 规范要求 
    
    // 手动写入初始提交文件以绕过哈希计算，确保为 00000000
    char filepath[MAX_PATH_LEN];
    sprintf(filepath, "%s/%s", COMMITS_DIR, c.hash);
    FILE* f = fopen(filepath, "w");
    fprintf(f, "hash:%s\nparent:\ntimestamp:%s\nmessage:%s\n", c.hash, c.timestamp, c.message);
    fclose(f);

    // Master branch
    char master_path[MAX_PATH_LEN];
    sprintf(master_path, "%s/master", BRANCHES_DIR);
    write_file_content(master_path, c.hash, 0, strlen(c.hash));

    // HEAD
    write_file_content(HEAD_FILE, "master", 0, 6);
}

void cmd_add(const char* filename) { // 
    if (!file_exists(GITLET_DIR)) {
        printf("Not in an initialized Gitlet directory.\n");
        return;
    }
    if (!file_exists(filename)) {
        printf("File does not exist.\n");
        return;
    }

    // Read and Hash file
    char* content = read_file_content(filename);
    char hash[HASH_LEN];
    compute_hash(content, hash);
    
    // Save Blob
    char blob_path[MAX_PATH_LEN];
    sprintf(blob_path, "%s/%s", BLOBS_DIR, hash);
    write_file_content(blob_path, content, 1, strlen(content)); // 

    // Update Staging (Append +filename)
    // 简化实现：追加到 staging.txt。Commit 时再处理去重
    FILE* f = fopen(STAGING_FILE, "a");
    fprintf(f, "+%s %s\n", filename, hash);
    fclose(f);
    
    free(content);
}

void cmd_commit(const char* msg) { // 
    if (strlen(msg) == 0) {
        printf("Please enter a commit message.\n");
        return;
    }
    
    if (!file_exists(STAGING_FILE)) {
        printf("No changes added to the commit.\n");
        return;
    }
    
    // 检查暂存区是否为空 (检查文件大小)
    FILE* stg_check = fopen(STAGING_FILE, "r");
    fseek(stg_check, 0, SEEK_END);
    if (ftell(stg_check) == 0) {
        fclose(stg_check);
        printf("No changes added to the commit.\n");
        return;
    }
    fclose(stg_check);

    char parent_hash[HASH_LEN];
    get_head_commit_hash(parent_hash);

    Commit parent, new_c;
    memset(&parent, 0, sizeof(Commit));
    memset(&new_c, 0, sizeof(Commit));

    if (strlen(parent_hash) > 0) {
        load_commit(parent_hash, &parent);
    }
    
    // 继承父提交的文件 
    new_c.file_count = parent.file_count;
    for(int i=0; i<parent.file_count; i++) {
        new_c.files[i] = parent.files[i];
    }

    // 应用暂存区变更
    FILE* f = fopen(STAGING_FILE, "r");
    char line[MAX_PATH_LEN + HASH_LEN + 5];
    while(fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char type = line[0];
        char* name = line + 1;
        char* hash_ptr = strchr(name, ' ');
        if (hash_ptr) *hash_ptr = 0; // split name and hash
        char* hash = hash_ptr ? hash_ptr + 1 : "";

        if (type == '+') {
            // Update or Add
            int found = 0;
            for(int i=0; i<new_c.file_count; i++) {
                if(strcmp(new_c.files[i].path, name) == 0) {
                    strcpy(new_c.files[i].blob_hash, hash);
                    found = 1; break;
                }
            }
            if (!found) {
                strcpy(new_c.files[new_c.file_count].path, name);
                strcpy(new_c.files[new_c.file_count].blob_hash, hash);
                new_c.file_count++;
            }
        } else if (type == '-') {
            // Remove: 移动数组元素覆盖
             for(int i=0; i<new_c.file_count; i++) {
                if(strcmp(new_c.files[i].path, name) == 0) {
                    // 将最后一个移到这里覆盖
                    new_c.files[i] = new_c.files[new_c.file_count - 1];
                    new_c.file_count--;
                    break;
                }
             }
        }
    }
    fclose(f);

    // 完成新提交对象
    strcpy(new_c.parent, parent_hash);
    get_timestamp(new_c.timestamp);
    strcpy(new_c.message, msg);
    
    // 计算Hash并保存
    save_commit(&new_c);
    
    // 更新分支并清空暂存区
    update_head_branch(new_c.hash);
    fclose(fopen(STAGING_FILE, "w")); // 清空文件
}

void cmd_log() { // 
    char current_hash[HASH_LEN];
    get_head_commit_hash(current_hash);

    Commit c;
    while(strlen(current_hash) > 0) {
        if (!load_commit(current_hash, &c)) break;
        printf("===\n");
        printf("commit %s\n", c.hash);
        printf("Date: %s\n", c.timestamp);
        printf("%s\n\n", c.message);
        
        strcpy(current_hash, c.parent); // 向上遍历
    }
}

void cmd_global_log() { // 
    WIN32_FIND_DATA findData;
    char searchPath[MAX_PATH_LEN];
    sprintf(searchPath, "%s/*", COMMITS_DIR);
    
    HANDLE hFind = FindFirstFile(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        
        Commit c;
        if (load_commit(findData.cFileName, &c)) {
            printf("===\n");
            printf("commit %s\n", c.hash);
            printf("Date: %s\n", c.timestamp);
            printf("%s\n\n", c.message);
        }
    } while (FindNextFile(hFind, &findData));
    FindClose(hFind);
}

void cmd_rm(const char* filename) { // 
    // 添加移除标记到暂存区
    FILE* f = fopen(STAGING_FILE, "a");
    fprintf(f, "-%s\n", filename);
    fclose(f);
    
    // 如果在工作区存在，则删除 (虽然 Gitlet 设计要求只在下次 Commit 时生效，但通常 rm 也删除工作区文件)
    // 严格按照文档 : "在下次提交时从工作目录中移除"，但如果现在在工作区有，通常也会删除。
    // 这里为了简单，只标记暂存。
    // 补充：Gitlet rm 描述说 "如果文件在HEAD提交中...标记为删除"。
    remove(filename); 
}

void cmd_find(const char* msg) { // 
    WIN32_FIND_DATA findData;
    char searchPath[MAX_PATH_LEN];
    sprintf(searchPath, "%s/*", COMMITS_DIR);
    HANDLE hFind = FindFirstFile(searchPath, &findData);
    int found = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Commit c;
            if (load_commit(findData.cFileName, &c)) {
                if (strcmp(c.message, msg) == 0) {
                    printf("%s\n", c.hash);
                    found = 1;
                }
            }
        } while (FindNextFile(hFind, &findData));
        FindClose(hFind);
    }
    
    if (!found) printf("Found no commit with that message.\n");
}

void cmd_status() { // 
    printf("=== Branches ===\n");
    char current_branch[MAX_PATH_LEN];
    FILE* f = fopen(HEAD_FILE, "r");
    if(f) {
        fgets(current_branch, sizeof(current_branch), f);
        current_branch[strcspn(current_branch, "\n")] = 0;
        fclose(f);
    }
    
    // 遍历分支目录
    WIN32_FIND_DATA findData;
    char searchPath[MAX_PATH_LEN];
    sprintf(searchPath, "%s/*", BRANCHES_DIR);
    HANDLE hFind = FindFirstFile(searchPath, &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (strcmp(findData.cFileName, current_branch) == 0)
                printf("*%s\n", findData.cFileName);
            else
                printf("%s\n", findData.cFileName);
        } while (FindNextFile(hFind, &findData));
        FindClose(hFind);
    }
    
    printf("\n=== Staged Files ===\n");
    // 解析暂存区显示 + 的文件
    if (file_exists(STAGING_FILE)) {
         FILE* stg = fopen(STAGING_FILE, "r");
         char line[MAX_PATH_LEN];
         while(fgets(line, sizeof(line), stg)) {
             if (line[0] == '+') {
                 char* space = strchr(line, ' ');
                 if(space) *space = 0;
                 printf("%s\n", line+1);
             }
         }
         fclose(stg);
    }

    printf("\n=== Removed Files ===\n");
    // 解析暂存区显示 - 的文件
    if (file_exists(STAGING_FILE)) {
         FILE* stg = fopen(STAGING_FILE, "r");
         char line[MAX_PATH_LEN];
         while(fgets(line, sizeof(line), stg)) {
             if (line[0] == '-') {
                 line[strcspn(line, "\n")] = 0;
                 printf("%s\n", line+1);
             }
         }
         fclose(stg);
    }
    printf("\n=== Modifications Not Staged For Commit ===\n\n");
    printf("=== Untracked Files ===\n\n");
}

// 恢复具体文件的逻辑
void restore_file(Commit* c, const char* filename) {
    for (int i = 0; i < c->file_count; i++) {
        if (strcmp(c->files[i].path, filename) == 0) {
            char blob_path[MAX_PATH_LEN];
            sprintf(blob_path, "%s/%s", BLOBS_DIR, c->files[i].blob_hash);
            char* content = read_file_content(blob_path);
            if (content) {
                write_file_content(filename, content, 1, strlen(content));
                free(content);
            }
            return;
        }
    }
    printf("File does not exist in that commit.\n");
}

void cmd_checkout(int argc, char* argv[]) { // 
    if (argc == 3 && strcmp(argv[1], "--") == 0) {
        // checkout -- file
        char hash[HASH_LEN];
        get_head_commit_hash(hash);
        Commit c;
        if(load_commit(hash, &c)) restore_file(&c, argv[2]);
    } 
    else if (argc == 4 && strcmp(argv[2], "--") == 0) {
        // checkout commit_id -- file
        Commit c;
        if (load_commit(argv[1], &c)) {
            restore_file(&c, argv[3]);
        } else {
            printf("No commit with that id exists.\n");
        }
    }
    else if (argc == 2) {
        // checkout branch
        char branch_path[MAX_PATH_LEN];
        sprintf(branch_path, "%s/%s", BRANCHES_DIR, argv[1]);
        if (!file_exists(branch_path)) {
            printf("No such branch exists.\n");
            return;
        }
        
        char current_branch[MAX_PATH_LEN];
        FILE* f = fopen(HEAD_FILE, "r");
        fgets(current_branch, sizeof(current_branch), f);
        fclose(f);
        current_branch[strcspn(current_branch, "\n")] = 0;
        if (strcmp(current_branch, argv[1]) == 0) {
            printf("No need to checkout the current branch.\n");
            return;
        }

        // 切换分支：加载目标分支的 Head Commit
        char hash[HASH_LEN];
        f = fopen(branch_path, "r");
        fgets(hash, HASH_LEN+1, f);
        fclose(f);

        Commit c;
        if(load_commit(hash, &c)) {
            // 覆盖所有文件
            for(int i=0; i<c.file_count; i++) {
                 char blob_path[MAX_PATH_LEN];
                 sprintf(blob_path, "%s/%s", BLOBS_DIR, c.files[i].blob_hash);
                 char* content = read_file_content(blob_path);
                 if (content) {
                     write_file_content(c.files[i].path, content, 1, strlen(content));
                     free(content);
                 }
            }
        }
        
        // 更新 HEAD
        write_file_content(HEAD_FILE, argv[1], 0, strlen(argv[1]));
        
        // 清空暂存区
        fclose(fopen(STAGING_FILE, "w"));
    }
}

void cmd_branch(const char* name) { // 
    char path[MAX_PATH_LEN];
    sprintf(path, "%s/%s", BRANCHES_DIR, name);
    if (file_exists(path)) {
        printf("A branch with that name already exists.\n");
        return;
    }
    char hash[HASH_LEN];
    get_head_commit_hash(hash);
    write_file_content(path, hash, 0, strlen(hash));
}

void cmd_rm_branch(const char* name) { // 
    char current_branch[MAX_PATH_LEN];
    FILE* f = fopen(HEAD_FILE, "r");
    fgets(current_branch, sizeof(current_branch), f);
    fclose(f);
    current_branch[strcspn(current_branch, "\n")] = 0;

    if (strcmp(current_branch, name) == 0) {
        printf("Cannot remove the current branch.\n");
        return;
    }

    char path[MAX_PATH_LEN];
    sprintf(path, "%s/%s", BRANCHES_DIR, name);
    if (!file_exists(path)) {
        printf("A branch with that name does not exist.\n");
    } else {
        remove(path);
    }
}

void cmd_reset(const char* commit_id) { // 
    Commit c;
    if (!load_commit(commit_id, &c)) {
        printf("No commit with that id exists.\n");
        return;
    }
    
    // Checkout all files
    for(int i=0; i<c.file_count; i++) {
        char blob_path[MAX_PATH_LEN];
        sprintf(blob_path, "%s/%s", BLOBS_DIR, c.files[i].blob_hash);
        char* content = read_file_content(blob_path);
        if (content) {
            write_file_content(c.files[i].path, content, 1, strlen(content));
            free(content);
        }
    }
    
    update_head_branch(commit_id);
    fclose(fopen(STAGING_FILE, "w"));
}

void cmd_merge(const char* branch_name) { // 
    char stg_path[MAX_PATH_LEN];
    FILE* stg_check = fopen(STAGING_FILE, "r");
    if (stg_check) {
        fseek(stg_check, 0, SEEK_END);
        if (ftell(stg_check) > 0) {
            printf("You have uncommitted changes.\n");
            fclose(stg_check); return;
        }
        fclose(stg_check);
    }

    char branch_path[MAX_PATH_LEN];
    sprintf(branch_path, "%s/%s", BRANCHES_DIR, branch_name);
    if (!file_exists(branch_path)) {
        printf("A branch with that name does not exist.\n");
        return;
    }
    
    char current_branch[MAX_PATH_LEN];
    FILE* f = fopen(HEAD_FILE, "r");
    fgets(current_branch, sizeof(current_branch), f);
    fclose(f);
    current_branch[strcspn(current_branch, "\n")] = 0;
    if (strcmp(current_branch, branch_name) == 0) {
        printf("Cannot merge a branch with itself.\n");
        return;
    }

    // 简化合并逻辑：直接拉取目标分支的文件 
    // 真实合并需要寻找 LCA，这里实现文档要求的简化版本
    char other_hash[HASH_LEN];
    f = fopen(branch_path, "r");
    fgets(other_hash, HASH_LEN+1, f);
    fclose(f);

    Commit other_c;
    load_commit(other_hash, &other_c);

    // 将目标分支的文件Checkout出来（模拟合并）
    for(int i=0; i<other_c.file_count; i++) {
         char blob_path[MAX_PATH_LEN];
         sprintf(blob_path, "%s/%s", BLOBS_DIR, other_c.files[i].blob_hash);
         char* content = read_file_content(blob_path);
         if (content) {
             write_file_content(other_c.files[i].path, content, 1, strlen(content));
             // Auto-stage
             cmd_add(other_c.files[i].path);
             free(content);
         }
    }

    // 创建 Merge Commit
    char msg[MAX_MSG_LEN];
    sprintf(msg, "Merged %s into %s.", branch_name, current_branch);
    cmd_commit(msg);
    
    // 更新 merge_parent (由于cmd_commit默认只设parent，这里需要手动Hack一下最新的commit)
    char head_hash[HASH_LEN];
    get_head_commit_hash(head_hash);
    Commit new_head;
    load_commit(head_hash, &new_head);
    strcpy(new_head.merge_parent, other_hash); // 设置第二个父节点 
    save_commit(&new_head);
}

/* ================= 主程序入口 ================= */
int main(int argc, char* argv[]) { // 
    if (argc < 2) {
        printf("Please enter a command.\n");
        return 0;
    }
    
    const char* cmd = argv[1];

    if (strcmp(cmd, "init") == 0) {
        cmd_init();
    }
    else if (strcmp(cmd, "add") == 0) {
        if (argc < 3) printf("Invalid args.\n");
        else cmd_add(argv[2]);
    }
    else if (strcmp(cmd, "commit") == 0) {
        if (argc < 3) printf("Please enter a commit message.\n"); // 
        else cmd_commit(argv[2]);
    }
    else if (strcmp(cmd, "rm") == 0) {
        if (argc < 3) printf("Invalid args.\n");
        else cmd_rm(argv[2]);
    }
    else if (strcmp(cmd, "log") == 0) {
        cmd_log();
    }
    else if (strcmp(cmd, "global-log") == 0) {
        cmd_global_log();
    }
    else if (strcmp(cmd, "find") == 0) {
        if (argc < 3) printf("Invalid args.\n");
        else cmd_find(argv[2]);
    }
    else if (strcmp(cmd, "status") == 0) {
        cmd_status();
    }
    else if (strcmp(cmd, "checkout") == 0) {
        // shift args to match helper function logic
        cmd_checkout(argc - 1, argv + 1); 
    }
    else if (strcmp(cmd, "branch") == 0) {
         if (argc < 3) printf("Invalid args.\n");
         else cmd_branch(argv[2]);
    }
    else if (strcmp(cmd, "rm-branch") == 0) {
         if (argc < 3) printf("Invalid args.\n");
         else cmd_rm_branch(argv[2]);
    }
    else if (strcmp(cmd, "reset") == 0) {
         if (argc < 3) printf("Invalid args.\n");
         else cmd_reset(argv[2]);
    }
    else if (strcmp(cmd, "merge") == 0) {
         if (argc < 3) printf("Invalid args.\n");
         else cmd_merge(argv[2]);
    }
    else {
        printf("No command with that name exists.\n");
    }

    return 0;
}