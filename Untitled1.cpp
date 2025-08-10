#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define NUM_THREADS 4

pthread_mutex_t lock;
int shared_sum = 0; // Bi?n dùng chung gi?a các thread

typedef struct {
    int thread_id;
    int result; // d? luu k?t qu? c?a m?i thread
} ThreadData;

void* worker(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    int local_result = data->thread_id * 10; // công vi?c c?a thread

    // Khóa mutex d? c?p nh?t bi?n chung
    pthread_mutex_lock(&lock);
    shared_sum += local_result;
    pthread_mutex_unlock(&lock);

    data->result = local_result;
    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    ThreadData thread_data[NUM_THREADS];

    // Kh?i t?o mutex
    if (pthread_mutex_init(&lock, NULL) != 0) {
        printf("Không th? kh?i t?o mutex\n");
        return 1;
    }

    // T?o các thread
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].thread_id = i + 1;
        if (pthread_create(&threads[i], NULL, worker, &thread_data[i]) != 0) {
            printf("L?i t?o thread %d\n", i);
            return 1;
        }
    }

    // Ch? các thread k?t thúc
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        printf("Thread %d tr? v? k?t qu?: %d\n", thread_data[i].thread_id, thread_data[i].result);
    }

    printf("T?ng giá tr? shared_sum: %d\n", shared_sum);

    // H?y mutex
    pthread_mutex_destroy(&lock);

    return 0;
}

