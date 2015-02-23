/* 
* @Author: largelyfs
* @Date:   2015-02-21 21:05:25
* @Last Modified by:   largelyfs
* @Last Modified time: 2015-02-23 14:08:34
*/

#include "pthread.h"
#include <iostream>



#define MAX_STRING_LENGTH 100
#define TABLE_SIZE 1e8
#define EXPTABLE_MAX_TABLE_SIZE 1000
#define EXPTABLE_MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000


using namespace std;
#include "Word2Vec.h"
struct Word2vecWithInt{
	int id;
	Word2Vec* w;
	Word2vecWithInt(Word2Vec* w, int id):id(id),w(w){}
};

void* trainModelThread(void* id){
	Word2vecWithInt* data = (Word2vecWithInt*)(id);
	Word2Vec* w = data->w;
	RandomGen *localr = new RandomGen(data->id);
	long long sen[MAX_SENTENCE_LENGTH];
	FileReader * localf = new FileReader(w->filename, MAX_STRING_LENGTH, w->filesize / (long long)(w->thread_number) * data->id);
	char buf[MAX_STRING_LENGTH];
	long long word_count, last_word_count;
	word_count = 0;last_word_count = 0;
	double alpha = w->alpha;
	double min_alpha = w->min_alpha;
	long long total_words = w->total_words;
	clock_t now;
	clock_t  start = w->start;
	long long sentence_len = 0, sentence_pos = 0, word_index;
	char ss[5]= "<\\s>";
	long long skipline =  w->v->searchWord(ss);
	long long now_word, last_word;
	Embedding * work = new Embedding(w->layer1_size);
	while (true){
		if (word_count - last_word_count > 10000){
			w->word_counts_actual += word_count - last_word_count;
			last_word_count = word_count;
			alpha = w->alpha * (1- w->word_counts_actual / (double)(total_words+1));
			if (alpha < min_alpha) alpha = min_alpha;
		}

		if (sentence_len==0){
			//Collect the words
			while (true){
				if (localf->hasWord()==0) break;
				localf->getWord(buf);
				word_index = w->v->searchWord(buf);
				if (word_index==-1) continue;
				word_count++;
				if (word_index==skipline) break;
				//subsampling
				sen[sentence_len] = word_index;
				sentence_len ++;
				if (sentence_len > MAX_SENTENCE_LENGTH) break;
			}
			sentence_pos = 0;
		}
		if ((word_count >= total_words / w->thread_number) || (localf->hasWord()==false)) break;
		now_word = sen[sentence_pos];
		
		for (int i = 0; i < w->layer1_size; i++) (*work)[i] = 0.0;
		int reduce_window = (localr->Next()) % w->window_size;
		for (int j = reduce_window; j < reduce_window * 2 + 1 - reduce_window; j++)
			if (j!=w->window_size){
				last_word = sentence_pos - w->window_size + j;
				if (last_word < 0 ) continue;
				if (last_word >= sentence_len) continue;
				last_word = sen[last_word];
				if (last_word==-1) continue;
				Embedding* e1 = w->senseembeddings[last_word][0];
				unsigned long long label, nextrandom;
				long long target;
				for (int d = 0; d < w->negative+1; d++){
					if (d==0){
						target = now_word;
						label = 1;
					}else{
						nextrandom = localr->Next();
						target = (w->table)[(nextrandom >> 16) % (w->tablesize)];
						if (target == 0) target = nextrandom % (w->word_number - 1) + 1;
						if (target == now_word) continue;
						label = 0;
					}
					Embedding* e2 = w->globalembeddings[target];
					double f = e1->Dot(*e2);
					double g;
					if (f > EXPTABLE_MAX_EXP) g = (label - 1) * alpha;
					else if (f < -EXPTABLE_MAX_EXP)  g = (label - 0) * alpha;
					else g = ( label - (*(w->e))[(int)((f + EXPTABLE_MAX_EXP) * (EXPTABLE_MAX_TABLE_SIZE / EXPTABLE_MAX_EXP / 2))]) * alpha;
						//work+= g * global;
					work->Saxpy((*e2), g);
						//global += g * sense
					e2->Saxpy((*e1), g);
				}
				//sense += work
				e1->Saxpy((*work), 1.0);
			}
		sentence_pos++;
		if (sentence_pos >= sentence_len){
			sentence_len = 0;
			continue;
		}
	}
	delete work;
	delete localr;
	delete localf;
	pthread_exit(NULL);
}

Word2Vec::Word2Vec(	const char* filename, int min_count=4, 
					int window=5, int size=100, double alpha=0.25, 
					double min_alpha=0.001, int negative = 5,
					int thread_number = 1){
	this->filename = new char[MAX_STRING_LENGTH];
	strcpy(this->filename, filename);
	this->v = new VocabGen(filename, MAX_STRING_LENGTH);
	this->r = new RandomGen();
	this->e = new ExpTable(EXPTABLE_MAX_TABLE_SIZE, EXPTABLE_MAX_EXP);
	this->min_count = min_count;
	this->window_size = window;
	this->alpha = alpha;
	this->min_alpha = min_alpha;
	this->layer1_size = size;
	this->negative = negative;
	this->tablesize = TABLE_SIZE;
	this->table = NULL;
	this->thread_number = thread_number;
	this->v->buildVocab();
	this->v->reduceVocab(this->min_count);
	this->filesize = this->v->fileSize();
	this->word_number = this->v->size();
	this->total_words = this->v->totalWords();
	this->resetWeights();
	this->inittable();
	this->trainModel();
}

Word2Vec::~Word2Vec(){
	if (this->filename!=NULL) delete this->filename;
	if (v!=NULL) delete v;
	if (r!=NULL) delete r;
	if (e!=NULL) delete e;
	if (this->table!=NULL) delete this->table;
	int l = this->globalembeddings.size();
	for (int i = 0; i < l; i++)
		if (this->globalembeddings[i] != NULL) delete this->globalembeddings[i];
	int li, lj;
	li = this->senseembeddings.size();
	for (int i = 0; i < li; i++){
		lj = this->senseembeddings[i].size();
		for (int j = 0; j < lj; j++) 
			if (this->senseembeddings[i][j] != NULL) delete this->senseembeddings[i][j];
	}
	li = this->clusterembeddings.size();
	for (int i = 0; i < li; i++){
		lj = this->clusterembeddings[i].size();
		for (int j = 0; j < lj; j++)
			if (this->clusterembeddings[i][j] != NULL) delete this->clusterembeddings[i][j];
	}
	this->clusternumber.clear();
	this->wordfreq.clear();
}

void Word2Vec::resetWeights(){
	this->globalembeddings.clear();
	this->senseembeddings.clear();
	this->clusterembeddings.clear();
	this->clusternumber.clear();
	this->wordfreq.clear();
	for (int i = 0; i < this->word_number; i++){
		this->globalembeddings.push_back(new Embedding(this->layer1_size));
		std::vector< Embedding* > v;
		v.clear();v.push_back(new Embedding(this->layer1_size));
		this->senseembeddings.push_back(v);
		std::vector<Embedding* > v1;
		v1.clear();v1.push_back(new Embedding(this->layer1_size));
		this->clusterembeddings.push_back(v1);
		std::vector<long long> v2;
		v2.clear();v2.push_back(0);
		this->wordfreq.push_back(v2);
		this->clusternumber.push_back(0);
	}
	//generate the random weights
	for (int i = 0; i < this->word_number; i++){
		this->globalembeddings[i]->randomGenerate(*(this->r));
		this->clusterembeddings[i][0]->randomGenerate(*(this->r));
		this->senseembeddings[i][0]->randomGenerate(*(this->r));
	}
	// this->globalembeddings[0]->show();
	// this->senseembeddings[0][0]->show();
	// this->clusterembeddings[0][0]->show();
}

void Word2Vec::inittable(){
	int a, i;
	double train_words_pow = 0;
	double d1, power = 0.75;
	this->table = new int[this->tablesize];
	for (a = 0; a < this->word_number; a++) train_words_pow += pow(this->v->searchWordCnt(a), power);
	i = 0;
	d1 = pow(this->v->searchWordCnt(i), power) / train_words_pow;
	for (a = 0; a < this->tablesize; a++){
		this->table[a] = i;
		if (a / double(this->tablesize) > d1){
			i++;
			d1 += pow(this->v->searchWordCnt(i), power) / train_words_pow;
		}
		if (i >= this->word_number) i = this->word_number -1;
	}
}

void Word2Vec::saveModel(const char* filename){
	FILE *fo;
	fo = fopen(filename,"wb");
	fprintf(fo, "%lld %d\n", this->word_number, this->layer1_size);
	for (int i = 0; i < this->word_number; i++){
		fprintf(fo, "%s ", this->v->searchWordContent(i).c_str());
		for (int j = 0; j < this->layer1_size; j++)
			fprintf(fo,"%lf ", (*(this->globalembeddings[i]))[j]);
		fprintf(fo, "\n");
		for (int num = 0; num < this->clusterembeddings[i].size(); num++){
			for (int j = 0; j< this->layer1_size; j++)
				fprintf(fo, "%lf ", (*(this->senseembeddings[i][num]))[j]);
			fprintf(fo, "\n");
		}
	}
}

void Word2Vec::trainModel(){
	pthread_t *pt = new pthread_t[this->thread_number];
	Word2vecWithInt** data = new Word2vecWithInt*[this->thread_number];
	for (int i = 0; i < this->thread_number; i++){
		data[i] = new Word2vecWithInt(this, i);
	}
	for (int i = 0; i < this->thread_number; i++) pthread_create(&pt[i], NULL, trainModelThread, (void*)(data[i]));
	for (int i = 0; i < this->thread_number; i++) pthread_join(pt[i], NULL);
	for (int i = 0; i < this->thread_number; i++) delete data[i];
	delete data;
	delete pt;
}


int main(){
	Word2Vec *w = new Word2Vec("text8",4);
	w->saveModel("output.txt");
	delete w;
    return 0;
}