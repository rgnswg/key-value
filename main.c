#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NOT_FOUND ((uint64_t)-1)
#define DB_NAME "database.db"
#define MAGIC_NUMBER 123

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
	char data[]; //key+len
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
	hashTable *t = malloc(sizeof(hashTable));
	t->size = size;
	t->elems = 0;
	t->buckets = calloc(size, sizeof(node*));
	return t;
}

uint64_t hashFunc(const char* str, uint32_t keylen, int tableSize){
	uint64_t hash=5381;
	int c;
	for(int i=0; i<keylen; i++){
		hash = ((hash << 5) + hash) + str[i];
	}
	return hash % tableSize;
}

int insert(hashTable* t, const char *key, int keylen, int offset){
	uint64_t index = hashFunc(key,keylen,t->size);
	//buscar si el valor ya existe, y pisar offset (supuestamente el ultimo valor escrito es el que vale)
	node* aux = t->buckets[index];
	while(aux!=NULL){
		if((strcmp(aux->key,key))==0){
			//si ya existe solo piso el offset
			aux->offset = offset;
			return 0;
		}
		aux = aux->next;
	}

	node * newNode = malloc(sizeof(node));
	newNode->key = malloc(sizeof(keylen)+1);
	memcpy(newNode->key, key, keylen);
    	newNode->key[keylen] = '\0';

   	newNode->offset = offset;

	newNode->next = t->buckets[index];
    	t->buckets[index] = newNode;
	t->elems++;
	return 0;
}

int delete(hashTable* t, const char* key){
	uint32_t keylen = strlen(key);
	uint64_t index = hashFunc(key,keylen,t->size);
	node *prev=NULL;
	node *aux = t->buckets[index];

	while(aux!=NULL){
		if(strcmp(key,aux->key)==0){
			if(prev==NULL){
				t->buckets[index] = aux->next;
			} else{
				prev->next = aux->next;
			}
			free(aux->key);
			free(aux);
			return 0;
		}
		prev = aux;
		aux = aux->next;
	}
	t->elems--;
	return 1;
}

uint64_t lookup(hashTable* t, const char *key){
	uint32_t keylen = strlen(key);
	uint64_t index = hashFunc(key,keylen,t->size);
	node *aux = t->buckets[index];

	while(aux!=NULL){
		if(strcmp(key,aux->key)==0) return aux->offset;
		aux = aux->next;
	}
	return NOT_FOUND;
}


uint64_t timestamp(){
	return (uint64_t)time(NULL);
}
int writeDown(FILE * a, char key[]){
	uint32_t keylen = strlen(key);
	uint32_t vallen = 0;
	uint64_t tstamp = timestamp();
	uint8_t status = 0x00;
	fseek(a,0,SEEK_END);
	fwrite(&tstamp,sizeof(uint64_t),1,a);
	fwrite(&status,sizeof(uint8_t),1,a);
	fwrite(&keylen,sizeof(uint32_t),1,a);
	fwrite(&vallen,sizeof(uint32_t),1,a);
	fwrite(key,sizeof(char),keylen,a);
	return 0;
}

int writeEntry(FILE * a, char key[], char val[]){
	uint32_t keylen = strlen(key);
	uint32_t vallen = strlen(val);
	uint64_t tstamp = timestamp();
	uint8_t status = 0x01;
	//fseek(a,0,SEEK_END);

	fwrite(&tstamp,sizeof(uint64_t),1,a);
	fwrite(&status,sizeof(uint8_t),1,a);
	fwrite(&keylen,sizeof(uint32_t),1,a);
	fwrite(&vallen,sizeof(uint32_t),1,a);
	fwrite(key,sizeof(char),keylen,a);
	fwrite(val,sizeof(char),vallen,a);
	return 0;
}

entry* readEntry(FILE* a, uint64_t offset){
	//tratar de leer el struct me dio problemas (culpa del padding)
	//hare la lectura campo a campo
	uint64_t tstamp;
	uint8_t stat;
	uint32_t klen;
	uint32_t vlen;
	//si el valor de offset es negativo, lo ignoro y no hago seek (para inicializar)
	if(offset>=0) fseek(a,offset,SEEK_SET);
	if(fread(&tstamp,sizeof(uint64_t),1,a) !=1) return NULL;// no pudo leer
	fread(&stat,sizeof(uint8_t),1,a);
	fread(&klen,sizeof(uint32_t),1,a);
	fread(&vlen,sizeof(uint32_t),1,a);
	uint32_t tlen = vlen + klen; //previendo caracter \n

	entry * e = malloc(sizeof(entry)+tlen);
	e->timestamp = tstamp;
	e->status = stat;
	e->keylen = klen;
	e->vallen = vlen;

	fread(e->data,sizeof(char),tlen,a);
	return e;
}
int writeHeader(FILE** a,hashTable*t){
	fileHeader h = {0};
	h.magicNumber = MAGIC_NUMBER;
	h.lastSize = t->size;
	h.entryCount = t->elems;
	h.density = t->elems / t->size;
	int lastpos = ftell(*a);
	fclose(*a);

	*a = fopen(DB_NAME,"rb+");
	fseek(*a,0,SEEK_SET);
	fwrite(&h,sizeof(h),1,*a);
	fclose(*a);
	*a = fopen(DB_NAME,"ab+");
	fseek(*a,lastpos,SEEK_SET);
	return 0;
}

fileHeader readHeader(FILE *a){
	fileHeader h={0};
	fseek(a,0,SEEK_SET);
	if(fread(&h, sizeof(fileHeader),1,a)!=1) h.magicNumber = 0;
	return h;
}

int getTableSize(FILE*a){
	fileHeader h = readHeader(a);
	if (h.density>=0.75) return (h.lastSize)*2;
	return h.lastSize;
}


int compact(FILE** a, hashTable *t){
	printf("[Compact] Deleting unused entries.\n");
	FILE * new = fopen("database.db.tmp","ab+");
	if(new == NULL) return 1;
	fileHeader h = readHeader(*a);
	fwrite(&h,sizeof(fileHeader),1,new);
	for(int i=0; i<t->size;i++){
		node *aux=t->buckets[i];
		while(aux!=NULL){
			entry *e = readEntry(*a,aux->offset);
			char *auxkey = malloc(e->keylen);
			memcpy(auxkey,e->data,e->keylen);
			writeEntry(new,auxkey,e->data+e->keylen);
			free(auxkey);
			aux = aux->next;
		}
	}
	fclose(new);
	fclose(*a);
	if (remove(DB_NAME)!=0) return 1;
	if (rename("database.db.tmp",DB_NAME)!=0) return 1;
	*a = fopen(DB_NAME,"ab+");
	if(*a==NULL){
		printf("[ERROR] Cant open file.");
		return 1;
	}
	return 0;
}

int printEntryJson(entry* e){
	if(e==NULL){
		printf("[ERROR] Cant print null entry.\n");
	}
	printf("{\n\"KEY\": \"%.*s\"\n\"VALUE\": \"%s\"\n}\n",e->keylen,e->data,e->data+e->keylen);
	return 0;
}

hashTable* genTable(FILE* a, int size){
	//printf(">Bootstraping hash table...\n");
	hashTable *t = createTable(size);
	fseek(a,sizeof(fileHeader),SEEK_SET);
	int lastPos=ftell(a);
	while(1){
		lastPos = ftell(a);
		entry * e = readEntry(a, -1);
		if (e==NULL) break;
		if (e->status == 0x01){
			printf("[BOOTSTRAP]  KEY: %.*s, OFFSET: %d\n",e->keylen,e->data,lastPos);
			char *auxkey =malloc(e->keylen);
			memcpy(auxkey, e->data,e->keylen);
			insert(t,auxkey,e->keylen,lastPos);
			free(auxkey);
		}
		if(e->status == 0x00){
			char *auxkey =malloc(e->keylen);
			memcpy(auxkey, e->data,e->keylen);
			delete(t,auxkey);
			printf("[BOOTSTRAP]  Tombstone found for: %s\n",auxkey);
			free(auxkey);
		}
	}
	return t;
}


int set(FILE* a, hashTable* t, char key[], char val[]){
	//logica de insercion respetando coherencia tabla-archivo
	printf("[SET]  Adding/updating key %s.\n",key);
	fseek(a,0,SEEK_END);
	long offset = ftell(a);
	writeEntry(a,key,val);
	fflush(a);
	insert(t,key, strlen(key),offset);
	printf("[SET]  Done.\n");
	return 0;
}

entry* get(FILE* a, hashTable *t, char key[]){
	printf("[GET]  Fetching key %s\n",key);
	uint64_t offset = lookup(t,key);
	if (offset == NOT_FOUND){
		printf("[GET]  Key not found.\n");
		return NULL;
	}
	printf("[GET]  Key found!\n");
	return readEntry(a,lookup(t,key));
}

int del(FILE* a, hashTable *t, char key[]){
	printf("[DEL] Deleting key %s.\n",key);
	fseek(a,0,SEEK_END);
	fflush(a);
	writeDown(a,key);
	delete(t,key);
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
	int existent = 0; //si el archivo existe valdra 1;
	FILE *arch = NULL;
	arch = fopen(DB_NAME,"rb");
	if(arch!=NULL){existent = 1; fclose(arch);}

	arch = fopen(DB_NAME,"ab+");
	if(arch == NULL){
		printf("[ERROR] Cant open file.");
		return 1;
	}
	if(!existent){
		fileHeader h = {0};
		h.lastSize = 100;// aca establezo el valor default que tendra una ht
		h.entryCount = 0;
		h.density =0;
		fwrite(&h,sizeof(h),1,arch);
	}

	hashTable * t = genTable(arch, getTableSize(arch));

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
					free(e); 				}
			} else {
				printf("[ERROR] Sintaxis: GET <key>\n");
			}

		} else if (strcmp(cmd, "DEL") == 0) {
			char *key = strtok(NULL, " ");
			if (key) {
				del(arch, t, key);
			} else {
				printf("[ERROR] Sintaxis: DEL <key>\n");
			}

		} else if (strcmp(cmd, "COMPACT") == 0) {
			compact(&arch, t);

		} else if (strcmp(cmd, "EXIT") == 0) {
			printf("Exiting... bye!\n");
			break;

		} else {
			printf("[ERROR] Unknown command: %s\n", cmd);
		}
	}

	writeHeader(&arch,t);
	freeTable(t);
	fclose(arch);
	return 0;
}

