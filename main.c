#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NOT_FOUND ((uint64_t)-1)
#define DB_NAME "database.db"
#define DB_TMP_NAME "database.db.tmp"
#define MAGIC_NUMBER 123
#define DEFAULT_TABLE_SIZE 100

typedef struct{
	uint32_t magicNumber;
	uint32_t lastSize;
	uint64_t entryCount;
	float density;
	char reserved[44]; //padding
} __attribute__((packed)) fileHeader;

typedef struct{
	uint64_t timestamp;
	uint8_t status; //0x01 active, 0x00 tombstone
	uint32_t keylen;
	uint32_t vallen;
	char data[]; //key+val
} entry;

typedef struct node{
	char * key;
	uint64_t offset;
	struct node *next; //hago enlazamiento en caso de colisión
} node;

typedef struct{
	node **buckets;
	int size;
	int elems;
} hashTable;

hashTable * createTable(int size){
	if(size <= 0) size = DEFAULT_TABLE_SIZE;
	hashTable *t = malloc(sizeof(hashTable));
	if(t == NULL) return NULL;
	t->size = size;
	t->elems = 0;
	t->buckets = calloc((size_t)size, sizeof(node*));
	if(t->buckets == NULL){
		free(t);
		return NULL;
	}
	return t;
}

uint64_t hashFunc(const char* str, uint32_t keylen, int tableSize){
	uint64_t hash = 5381;
	for(uint32_t i = 0; i < keylen; i++){
		hash = ((hash << 5) + hash) + (unsigned char)str[i];
	}
	return hash % (uint64_t)tableSize;
}

int insert(hashTable* t, const char *key, uint32_t keylen, uint64_t offset){
	uint64_t index = hashFunc(key, keylen, t->size);
	//buscar si el valor ya existe, y pisar offset (supuestamente el ultimo valor escrito es el que vale)
	node* aux = t->buckets[index];
	while(aux != NULL){
		if(strlen(aux->key) == keylen && memcmp(aux->key, key, keylen) == 0){
			//si ya existe solo piso el offset
			aux->offset = offset;
			return 0;
		}
		aux = aux->next;
	}

	node * newNode = malloc(sizeof(node));
	if(newNode == NULL) return 1;
	newNode->key = malloc((size_t)keylen + 1);
	if(newNode->key == NULL){
		free(newNode);
		return 1;
	}
	memcpy(newNode->key, key, keylen);
	newNode->key[keylen] = '\0';

	newNode->offset = offset;

	newNode->next = t->buckets[index];
	t->buckets[index] = newNode;
	t->elems++;
	return 0;
}

int delete(hashTable* t, const char* key){
	uint32_t keylen = (uint32_t)strlen(key);
	uint64_t index = hashFunc(key, keylen, t->size);
	node *prev = NULL;
	node *aux = t->buckets[index];

	while(aux != NULL){
		if(strcmp(key, aux->key) == 0){
			if(prev == NULL){
				t->buckets[index] = aux->next;
			} else{
				prev->next = aux->next;
			}
			free(aux->key);
			free(aux);
			t->elems--;
			return 0;
		}
		prev = aux;
		aux = aux->next;
	}
	return 1;
}

uint64_t lookup(hashTable* t, const char *key){
	uint32_t keylen = (uint32_t)strlen(key);
	uint64_t index = hashFunc(key, keylen, t->size);
	node *aux = t->buckets[index];

	while(aux != NULL){
		if(strcmp(key, aux->key) == 0) return aux->offset;
		aux = aux->next;
	}
	return NOT_FOUND;
}


uint64_t timestamp(){
	return (uint64_t)time(NULL);
}

int writeDown(FILE * a, const char *key){
	uint32_t keylen = (uint32_t)strlen(key);
	uint32_t vallen = 0;
	uint64_t tstamp = timestamp();
	uint8_t status = 0x00;
	if(fseek(a, 0, SEEK_END) != 0) return 1;
	if(fwrite(&tstamp, sizeof(uint64_t), 1, a) != 1) return 1;
	if(fwrite(&status, sizeof(uint8_t), 1, a) != 1) return 1;
	if(fwrite(&keylen, sizeof(uint32_t), 1, a) != 1) return 1;
	if(fwrite(&vallen, sizeof(uint32_t), 1, a) != 1) return 1;
	if(fwrite(key, sizeof(char), keylen, a) != keylen) return 1;
	if(fflush(a) != 0) return 1;
	return 0;
}

int writeEntryRaw(FILE * a, uint64_t tstamp, const char *key, uint32_t keylen, const char *val, uint32_t vallen){
	uint8_t status = 0x01;
	if(fseek(a, 0, SEEK_END) != 0) return 1;
	if(fwrite(&tstamp, sizeof(uint64_t), 1, a) != 1) return 1;
	if(fwrite(&status, sizeof(uint8_t), 1, a) != 1) return 1;
	if(fwrite(&keylen, sizeof(uint32_t), 1, a) != 1) return 1;
	if(fwrite(&vallen, sizeof(uint32_t), 1, a) != 1) return 1;
	if(fwrite(key, sizeof(char), keylen, a) != keylen) return 1;
	if(fwrite(val, sizeof(char), vallen, a) != vallen) return 1;
	return 0;
}

int writeEntry(FILE * a, const char *key, const char *val){
	uint32_t keylen = (uint32_t)strlen(key);
	uint32_t vallen = (uint32_t)strlen(val);
	return writeEntryRaw(a, timestamp(), key, keylen, val, vallen);
}

entry* readEntry(FILE* a, int64_t offset){
	//tratar de leer el struct me dio problemas (culpa del padding)
	//hare la lectura campo a campo
	uint64_t tstamp;
	uint8_t stat;
	uint32_t klen;
	uint32_t vlen;
	//si el valor de offset es negativo, lo ignoro y no hago seek (para inicializar)
	if(offset >= 0){
		if(fseek(a, (long)offset, SEEK_SET) != 0) return NULL;
	}
	if(fread(&tstamp, sizeof(uint64_t), 1, a) != 1) return NULL; // no pudo leer
	if(fread(&stat, sizeof(uint8_t), 1, a) != 1) return NULL;
	if(fread(&klen, sizeof(uint32_t), 1, a) != 1) return NULL;
	if(fread(&vlen, sizeof(uint32_t), 1, a) != 1) return NULL;
	uint64_t tlen = (uint64_t)klen + (uint64_t)vlen;

	entry * e = malloc(sizeof(entry) + (size_t)tlen);
	if(e == NULL) return NULL;
	e->timestamp = tstamp;
	e->status = stat;
	e->keylen = klen;
	e->vallen = vlen;

	if(tlen > 0 && fread(e->data, sizeof(char), (size_t)tlen, a) != (size_t)tlen){
		free(e);
		return NULL;
	}
	return e;
}

int writeHeader(FILE *a, hashTable *t){
	fileHeader h = {0};
	h.magicNumber = MAGIC_NUMBER;
	h.lastSize = (uint32_t)t->size;
	h.entryCount = (uint64_t)t->elems;
	h.density = (t->size > 0) ? ((float)t->elems / (float)t->size) : 0.0f;

	long lastpos = ftell(a);
	if(fseek(a, 0, SEEK_SET) != 0) return 1;
	if(fwrite(&h, sizeof(h), 1, a) != 1) return 1;
	if(fflush(a) != 0) return 1;
	if(lastpos >= 0) fseek(a, lastpos, SEEK_SET);
	return 0;
}

fileHeader readHeader(FILE *a){
	fileHeader h = {0};
	fseek(a, 0, SEEK_SET);
	if(fread(&h, sizeof(fileHeader), 1, a) != 1) h.magicNumber = 0;
	return h;
}

int getTableSize(FILE *a){
	fileHeader h = readHeader(a);
	if(h.magicNumber != MAGIC_NUMBER){
		printf("[WARN] Invalid or missing magic number in header, using default table size.\n");
		return DEFAULT_TABLE_SIZE;
	}
	if(h.lastSize == 0) return DEFAULT_TABLE_SIZE;
	if(h.density >= 0.75f) return (int)(h.lastSize * 2);
	return (int)h.lastSize;
}


int compact(FILE** a, hashTable *t){
	printf("[Compact] Deleting unused entries.\n");
	FILE * newFile = fopen(DB_TMP_NAME, "wb+");
	if(newFile == NULL) return 1;

	fileHeader h = {0};
	h.magicNumber = MAGIC_NUMBER;
	h.lastSize = (uint32_t)t->size;
	h.entryCount = (uint64_t)t->elems;
	h.density = (t->size > 0) ? ((float)t->elems / (float)t->size) : 0.0f;
	if(fwrite(&h, sizeof(fileHeader), 1, newFile) != 1){
		fclose(newFile);
		remove(DB_TMP_NAME);
		return 1;
	}

	uint64_t *newOffsets = NULL;
	if(t->elems > 0){
		newOffsets = malloc((size_t)t->elems * sizeof(uint64_t));
		if(newOffsets == NULL){
			fclose(newFile);
			remove(DB_TMP_NAME);
			return 1;
		}
	}

	int idx = 0;
	for(int i = 0; i < t->size; i++){
		node *aux = t->buckets[i];
		while(aux != NULL){
			entry *e = readEntry(*a, (int64_t)aux->offset);
			if(e == NULL){
				free(newOffsets);
				fclose(newFile);
				remove(DB_TMP_NAME);
				return 1;
			}
			fseek(newFile, 0, SEEK_END);
			long newPos = ftell(newFile);
			if(newPos < 0 || writeEntryRaw(newFile, e->timestamp, e->data, e->keylen, e->data + e->keylen, e->vallen) != 0){
				free(e);
				free(newOffsets);
				fclose(newFile);
				remove(DB_TMP_NAME);
				return 1;
			}
			newOffsets[idx++] = (uint64_t)newPos;
			free(e);
			aux = aux->next;
		}
	}

	if(fflush(newFile) != 0 || fclose(newFile) != 0){
		free(newOffsets);
		remove(DB_TMP_NAME);
		return 1;
	}

	fclose(*a);
	if(rename(DB_TMP_NAME, DB_NAME) != 0){
		free(newOffsets);
		remove(DB_TMP_NAME);
		*a = fopen(DB_NAME, "rb+");
		return 1;
	}

	*a = fopen(DB_NAME, "rb+");
	if(*a == NULL){
		free(newOffsets);
		printf("[ERROR] Cant open file.\n");
		return 1;
	}

	idx = 0;
	for(int i = 0; i < t->size; i++){
		node *aux = t->buckets[i];
		while(aux != NULL){
			aux->offset = newOffsets[idx++];
			aux = aux->next;
		}
	}
	free(newOffsets);
	printf("[Compact] Done.\n");
	return 0;
}

int printEntryJson(entry* e){
	if(e == NULL){
		printf("[ERROR] Cant print null entry.\n");
		return 1;
	}
	printf("{\n  \"KEY\": \"%.*s\",\n  \"VALUE\": \"%.*s\"\n}\n",
		(int)e->keylen, e->data,
		(int)e->vallen, e->data + e->keylen);
	return 0;
}

hashTable* genTable(FILE* a, int size){
	//printf(">Bootstraping hash table...\n");
	hashTable *t = createTable(size);
	if(t == NULL) return NULL;
	fseek(a, (long)sizeof(fileHeader), SEEK_SET);
	while(1){
		long lastPos = ftell(a);
		if(lastPos < 0) break;
		entry * e = readEntry(a, -1);
		if(e == NULL) break;
		if(e->status == 0x01){
			printf("[BOOTSTRAP]  KEY: %.*s, OFFSET: %ld\n", (int)e->keylen, e->data, lastPos);
			insert(t, e->data, e->keylen, (uint64_t)lastPos);
		} else if(e->status == 0x00){
			char *auxkey = malloc((size_t)e->keylen + 1);
			if(auxkey != NULL){
				memcpy(auxkey, e->data, e->keylen);
				auxkey[e->keylen] = '\0';
				delete(t, auxkey);
				printf("[BOOTSTRAP]  Tombstone found for: %s\n", auxkey);
				free(auxkey);
			}
		}
		free(e);
	}
	return t;
}


int set(FILE* a, hashTable* t, const char *key, const char *val){
	//logica de insercion respetando coherencia tabla-archivo
	printf("[SET]  Adding/updating key %s.\n", key);
	if(fseek(a, 0, SEEK_END) != 0) return 1;
	long offset = ftell(a);
	if(offset < 0) return 1;
	if(writeEntry(a, key, val) != 0){
		printf("[ERROR] Failed to write entry.\n");
		return 1;
	}
	if(fflush(a) != 0) return 1;
	if(insert(t, key, (uint32_t)strlen(key), (uint64_t)offset) != 0){
		printf("[ERROR] Failed to insert into hash table.\n");
		return 1;
	}
	printf("[SET]  Done.\n");
	return 0;
}

entry* get(FILE* a, hashTable *t, const char *key){
	printf("[GET]  Fetching key %s\n", key);
	uint64_t offset = lookup(t, key);
	if(offset == NOT_FOUND){
		printf("[GET]  Key not found.\n");
		return NULL;
	}
	printf("[GET]  Key found!\n");
	return readEntry(a, (int64_t)offset);
}

int del(FILE* a, hashTable *t, const char *key){
	printf("[DEL] Deleting key %s.\n", key);
	if(lookup(t, key) == NOT_FOUND){
		printf("[DEL] Key not found.\n");
		return 1;
	}
	if(writeDown(a, key) != 0){
		printf("[ERROR] Failed to write tombstone.\n");
		return 1;
	}
	delete(t, key);
	printf("[DEL] Done.\n");
	return 0;
}

void freeTable(hashTable *t) {
	if (t == NULL) return;

	for (int i = 0; i < t->size; i++) {
		node *curr = t->buckets[i];
		while (curr != NULL) {
			node *aux = curr;
			curr = curr->next;
			free(aux->key);
			free(aux);
		}
	}
	free(t->buckets);
	free(t);
	printf("[SHUTDOWN] HashTable freed.\n");
}


int main(){
	FILE *arch = fopen(DB_NAME, "rb+");
	if(arch == NULL){
		arch = fopen(DB_NAME, "wb+");
		if(arch == NULL){
			printf("[ERROR] Cant open file.\n");
			return 1;
		}
		fileHeader h = {0};
		h.magicNumber = MAGIC_NUMBER;
		h.lastSize = DEFAULT_TABLE_SIZE; // aca establezco el valor default que tendra una ht
		h.entryCount = 0;
		h.density = 0.0f;
		if(fwrite(&h, sizeof(h), 1, arch) != 1 || fflush(arch) != 0){
			printf("[ERROR] Cant initialize header.\n");
			fclose(arch);
			return 1;
		}
	}

	hashTable * t = genTable(arch, getTableSize(arch));
	if(t == NULL){
		printf("[ERROR] Cant create hash table.\n");
		fclose(arch);
		return 1;
	}

	char linea[256];
	printf("==COMMANDS==\n  SET <key> <value> : set or update a key\n  GET <key> : fetch value of key\n  DEL <key> : delete key\n  COMPACT : compact file\n  EXIT : close database\n============\n");

	while(1){
		printf("\n> ");
		if (fgets(linea, sizeof(linea), stdin) == NULL) break;

		linea[strcspn(linea, "\n")] = '\0';

		char *cmd = strtok(linea, " ");
		if (cmd == NULL) continue;

		if (strcmp(cmd, "SET") == 0) {
			char *key = strtok(NULL, " ");
			char *val = strtok(NULL, "");
			if (key && val) {
				set(arch, t, key, val);
			} else {
				printf("[ERROR] Syntax: SET <key> <value>\n");
			}

		} else if (strcmp(cmd, "GET") == 0) {
			char *key = strtok(NULL, " ");
			if (key) {
				entry *e = get(arch, t, key);
				if (e) {
					printEntryJson(e);
					free(e);
				}
			} else {
				printf("[ERROR] Syntax: GET <key>\n");
			}

		} else if (strcmp(cmd, "DEL") == 0) {
			char *key = strtok(NULL, " ");
			if (key) {
				del(arch, t, key);
			} else {
				printf("[ERROR] Syntax: DEL <key>\n");
			}

		} else if (strcmp(cmd, "COMPACT") == 0) {
			if (compact(&arch, t) != 0) {
				printf("[ERROR] Compaction failed.\n");
				if (arch == NULL) break;
			}

		} else if (strcmp(cmd, "EXIT") == 0) {
			printf("Exiting... bye!\n");
			break;

		} else {
			printf("[ERROR] Unknown command: %s\n", cmd);
		}
	}

	if (arch != NULL) {
		writeHeader(arch, t);
		fclose(arch);
	}
	freeTable(t);
	return 0;
}

