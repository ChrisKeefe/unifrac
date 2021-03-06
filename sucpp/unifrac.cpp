#include "tree.hpp"
#include "biom.hpp"
#include "unifrac.hpp"
#include "affinity.hpp"
#include <unordered_map>
#include <cstdlib>
#include <thread>
#include <signal.h>
#include <stdarg.h>
#include <algorithm>
#include <pthread.h>

// embed in this file, to properly instantiate the templatized functions
#include "unifrac_task.cpp"


static pthread_mutex_t printf_mutex;
static bool* report_status;

std::string su::test_table_ids_are_subset_of_tree(su::biom &table, su::BPTree &tree) {
    std::unordered_set<std::string> tip_names = tree.get_tip_names();
    std::unordered_set<std::string>::const_iterator hit;
    std::string a_missing_name = "";

    for(auto i : table.obs_ids) {
        hit = tip_names.find(i);
        if(hit == tip_names.end()) {
            a_missing_name = i;
            break;
        }
    }

    return a_missing_name;
}

int sync_printf(const char *format, ...) {
    // https://stackoverflow.com/a/23587285/19741
    va_list args;
    va_start(args, format);

    pthread_mutex_lock(&printf_mutex);
    vprintf(format, args);
    pthread_mutex_unlock(&printf_mutex);

    va_end(args);
}

void sig_handler(int signo) {
    // http://www.thegeekstuff.com/2012/03/catch-signals-sample-c-code
    if (signo == SIGUSR1) {
        if(report_status == NULL)
            fprintf(stderr, "Cannot report status.\n");
        else {
            for(int i = 0; i < CPU_SETSIZE; i++) {
                report_status[i] = true;
            }
        }
    }
}

using namespace su;


PropStack::PropStack(uint32_t vecsize) {
    defaultsize = vecsize;
    prop_stack = std::stack<double*>();
    prop_map = std::unordered_map<uint32_t, double*>();

    prop_map.reserve(1000);
}

PropStack::~PropStack() {
    double *vec;
    // drain stack
    for(unsigned int i = 0; i < prop_stack.size(); i++) {
        vec = prop_stack.top();
        prop_stack.pop();
        free(vec);
    }

    // drain the map
    for(auto it = prop_map.begin(); it != prop_map.end(); it++) {
        vec = it->second;
        free(vec);
    }
    prop_map.clear();
}

double* PropStack::get(uint32_t i) {
    return prop_map[i];
}

void PropStack::push(uint32_t node) {
    double* vec = prop_map[node];
    prop_map.erase(node);
    prop_stack.push(vec);
}

double* PropStack::pop(uint32_t node) {
    /*
     * if we don't have any available vectors, create one
     * add it to our record of known vectors so we can track our mallocs
     */
    double *vec;
    int err = 0;
    if(prop_stack.empty()) {
        err = posix_memalign((void **)&vec, 32, sizeof(double) * defaultsize);
        if(vec == NULL || err != 0) {
            fprintf(stderr, "Failed to allocate %zd bytes, err %d; [%s]:%d\n",
                    sizeof(double) * defaultsize, err, __FILE__, __LINE__);
            exit(EXIT_FAILURE);
        }
    }
    else {
        vec = prop_stack.top();
        prop_stack.pop();
    }

    prop_map[node] = vec;
    return vec;
}

double** su::deconvolute_stripes(std::vector<double*> &stripes, uint32_t n) {
    // would be better to just do striped_to_condensed_form
    double **dm;
    dm = (double**)malloc(sizeof(double*) * n);
    if(dm == NULL) {
        fprintf(stderr, "Failed to allocate %zd bytes; [%s]:%d\n",
                sizeof(double*) * n, __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    for(unsigned int i = 0; i < n; i++) {
        dm[i] = (double*)malloc(sizeof(double) * n);
        if(dm[i] == NULL) {
            fprintf(stderr, "Failed to allocate %zd bytes; [%s]:%d\n",
                    sizeof(double) * n, __FILE__, __LINE__);
            exit(EXIT_FAILURE);
        }
        dm[i][i] = 0;
    }

    for(unsigned int i = 0; i < stripes.size(); i++) {
        double *vec = stripes[i];
        unsigned int k = 0;
        for(unsigned int row = 0, col = i + 1; row < n; row++, col++) {
            if(col < n) {
                dm[row][col] = vec[k];
                dm[col][row] = vec[k];
            } else {
                dm[col % n][row] = vec[k];
                dm[row][col % n] = vec[k];
            }
            k++;
        }
    }
    return dm;
}


void su::stripes_to_condensed_form(std::vector<double*> &stripes, uint32_t n, double* &cf, unsigned int start, unsigned int stop) {
    // n must be >= 2, but that should be enforced upstream as that would imply
    // computing unifrac on a single sample.

    uint64_t comb_N = comb_2(n);
    for(unsigned int stripe = start; stripe < stop; stripe++) {
        // compute the (i, j) position of each element in each stripe
        uint64_t i = 0;
        uint64_t j = stripe + 1;
        for(uint64_t k = 0; k < n; k++, i++, j++) {
            if(j == n) {
                i = 0;
                j = n - (stripe + 1);
            }
            // determine the position in the condensed form vector for a given (i, j)
            // based off of
            // https://docs.scipy.org/doc/scipy/reference/generated/scipy.spatial.distance.squareform.html
            uint64_t comb_N_minus_i = comb_2(n - i);
            cf[comb_N - comb_N_minus_i + (j - i - 1)] = stripes[stripe][k];
        }
    }
}

void progressbar(float progress) {
    // from http://stackoverflow.com/a/14539953
    //
    // could encapsulate into a classs for displaying time elapsed etc
    int barWidth = 70;
    std::cout << "[";
    int pos = barWidth * progress;
    for (int i = 0; i < barWidth; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << " %\r";
    std::cout.flush();
}

template<class TFloat>
void embed_proportions(TFloat* __restrict__ out, const double* __restrict__ in, unsigned int emb, uint32_t n_samples) {
   const uint64_t n_samples_r = ((n_samples + UNIFRAC_BLOCK-1)/UNIFRAC_BLOCK)*UNIFRAC_BLOCK; // round up
   const uint64_t offset = emb * n_samples_r;

   for(unsigned int i = 0; i < n_samples; i++) {
       out[offset + i] = in[i];
   }

   // avoid NaNs
   for(unsigned int i = n_samples; i < n_samples_r; i++) {
       out[offset + i] = 1.0;
   }
}

template<class TFloat>
void initialize_embedded(TFloat*& prop, unsigned int num, const su::task_parameters* task_p) {
    const unsigned int n_samples = task_p->n_samples;
    const uint64_t  n_samples_r = ((n_samples + UNIFRAC_BLOCK-1)/UNIFRAC_BLOCK)*UNIFRAC_BLOCK; // round up
    uint64_t bsize = n_samples_r * num;

    TFloat* buf = NULL;
    int err = posix_memalign((void **)&buf, 4096, sizeof(TFloat) * bsize);
    if(buf == NULL || err != 0) {
        fprintf(stderr, "Failed to allocate %zd bytes, err %d; [%s]:%d\n",
                sizeof(TFloat) * bsize, err, __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
#pragma acc enter data create(buf[:bsize])
   prop=buf;
}

template<class TFloat>
void initialize_sample_counts(TFloat*& _counts, const su::task_parameters* task_p, biom &table) {
    const unsigned int n_samples = task_p->n_samples;
    const uint64_t  n_samples_r = ((n_samples + UNIFRAC_BLOCK-1)/UNIFRAC_BLOCK)*UNIFRAC_BLOCK; // round up
    TFloat * counts = NULL;
    int err = 0;
    err = posix_memalign((void **)&counts, 4096, sizeof(TFloat) * n_samples_r);
    if(counts == NULL || err != 0) {
        fprintf(stderr, "Failed to allocate %zd bytes, err %d; [%s]:%d\n",
                sizeof(TFloat) * n_samples_r, err, __FILE__, __LINE__);
        exit(EXIT_FAILURE);
    }
    for(unsigned int i = 0; i < n_samples; i++) {
        counts[i] = table.sample_counts[i];
    }
   // avoid NaNs
   for(unsigned int i = n_samples; i < n_samples_r; i++) {
       counts[i] = 0.0;
   }

#pragma acc enter data copyin(counts[:n_samples_r])
   _counts=counts;
}

void initialize_stripes(std::vector<double*> &dm_stripes,
                        std::vector<double*> &dm_stripes_total,
                        bool want_total,
                        const su::task_parameters* task_p) {
    int err = 0;
    for(unsigned int i = task_p->start; i < task_p->stop; i++){
        err = posix_memalign((void **)&dm_stripes[i], 4096, sizeof(double) * task_p->n_samples);
        if(dm_stripes[i] == NULL || err != 0) {
            fprintf(stderr, "Failed to allocate %zd bytes, err %d; [%s]:%d\n",
                    sizeof(double) * task_p->n_samples, err, __FILE__, __LINE__);
            exit(EXIT_FAILURE);
        }
        for(unsigned int j = 0; j < task_p->n_samples; j++)
            dm_stripes[i][j] = 0.;

        if(want_total) {
            err = posix_memalign((void **)&dm_stripes_total[i], 4096, sizeof(double) * task_p->n_samples);
            if(dm_stripes_total[i] == NULL || err != 0) {
                fprintf(stderr, "Failed to allocate %zd bytes err %d; [%s]:%d\n",
                        sizeof(double) * task_p->n_samples, err, __FILE__, __LINE__);
                exit(EXIT_FAILURE);
            }
            for(unsigned int j = 0; j < task_p->n_samples; j++)
                dm_stripes_total[i][j] = 0.;
        }
    }
}

// Computes Faith's PD for the samples in  `table` over the phylogenetic
// tree given by `tree`.
// Assure that tree does not contain ids that are not in table
void su::faith_pd(biom &table,
                  BPTree &tree,
                  double* result) {
    PropStack propstack(table.n_samples);

    uint32_t node;
    double *node_proportions;
    double length;

    // for node in postorderselect
    for(unsigned int k = 0; k < (tree.nparens / 2) - 1; k++) {
        node = tree.postorderselect(k);
        // get branch length
        length = tree.lengths[node];

        // get node proportions and set intermediate scores
        node_proportions = propstack.pop(node);
        set_proportions(node_proportions, tree, node, table, propstack);

        for (unsigned int sample = 0; sample < table.n_samples; sample++){
            // calculate contribution of node to score
            result[sample] += (node_proportions[sample] > 0) * length;
        }
    }
}

template<class TaskT, class TFloat>
void unifracTT(biom &table,
               BPTree &tree,
               const bool want_total,
               std::vector<double*> &dm_stripes,
               std::vector<double*> &dm_stripes_total,
               const su::task_parameters* task_p) {
    int err;
    // processor affinity
#ifndef _OPENACC
    // processor affinity, when not using openacc
    err = bind_to_core(task_p->tid);
    if(err != 0) {
        fprintf(stderr, "Unable to bind thread %d to core: %d\n", task_p->tid, err);
        exit(EXIT_FAILURE);
    }
#endif

    if(table.n_samples != task_p->n_samples) {
        fprintf(stderr, "Task and table n_samples not equal\n");
        exit(EXIT_FAILURE);
    }
    const unsigned int n_samples = task_p->n_samples;
    const uint64_t  n_samples_r = ((n_samples + UNIFRAC_BLOCK-1)/UNIFRAC_BLOCK)*UNIFRAC_BLOCK; // round up


    PropStack propstack(table.n_samples);

    const unsigned int max_emb = 128;

    uint32_t node;
    double *node_proportions;
    TFloat *embedded_proportions;

    initialize_embedded<TFloat>(embedded_proportions, max_emb, task_p);
    initialize_stripes(std::ref(dm_stripes), std::ref(dm_stripes_total), want_total, task_p);

    TaskT taskObj(std::ref(dm_stripes), std::ref(dm_stripes_total),embedded_proportions,max_emb,task_p);

    TFloat *lengths = NULL;
    err = posix_memalign((void **)&lengths, 4096, sizeof(TFloat) * max_emb);
    if(err != 0) {
        fprintf(stderr, "posix_memalign(%d) failed: %d\n", sizeof(TFloat) * max_emb,  err);
        exit(EXIT_FAILURE);
    }
#pragma acc enter data create(lengths[:max_emb])

    unsigned int filled_emb = 0;

    for(unsigned int k = 0; k < (tree.nparens / 2) - 1; k++) {
        node = tree.postorderselect(k);
        lengths[filled_emb] = tree.lengths[node];

        node_proportions = propstack.pop(node);
        set_proportions(node_proportions, tree, node, table, propstack);

        if(task_p->bypass_tips && tree.isleaf(node))
            continue;

        embed_proportions<TFloat>(embedded_proportions, node_proportions, filled_emb, task_p->n_samples);
        filled_emb++;
        /*
         * The values in the example vectors correspond to index positions of an
         * element in the resulting distance matrix. So, in the example below,
         * the following can be interpreted:
         *
         * [0 1 2]
         * [1 2 3]
         *
         * As comparing the sample for row 0 against the sample for col 1, the
         * sample for row 1 against the sample for col 2, the sample for row 2
         * against the sample for col 3.
         *
         * In other words, we're computing stripes of a distance matrix. In the
         * following example, we're computing over 6 samples requiring 3
         * stripes.
         *
         * A; stripe == 0
         * [0 1 2 3 4 5]
         * [1 2 3 4 5 0]
         *
         * B; stripe == 1
         * [0 1 2 3 4 5]
         * [2 3 4 5 0 1]
         *
         * C; stripe == 2
         * [0 1 2 3 4 5]
         * [3 4 5 0 1 2]
         *
         * The stripes end up computing the following positions in the distance
         * matrix.
         *
         * x A B C x x
         * x x A B C x
         * x x x A B C
         * C x x x A B
         * B C x x x A
         * A B C x x x
         *
         * However, we store those stripes as vectors, ie
         * [ A A A A A A ]
         *
         * We end up performing N / 2 redundant calculations on the last stripe
         * (see C) but that is small over large N.
         */

        if (filled_emb==max_emb) {
#pragma acc wait
#pragma acc update device(embedded_proportions[:n_samples_r*filled_emb],lengths[:filled_emb])
          taskObj._run(filled_emb,lengths);
          filled_emb=0;

          if(__builtin_expect(report_status[task_p->tid], false)) {
            sync_printf("tid:%d\tstart:%d\tstop:%d\tk:%d\ttotal:%d\n", task_p->tid, task_p->start, task_p->stop, k, (tree.nparens / 2) - 1);
            report_status[task_p->tid] = false;
          }
        }
    }

    if (filled_emb>0) {
#pragma acc wait
#pragma acc update device(embedded_proportions[:n_samples_r*filled_emb],lengths[:filled_emb])
          taskObj._run(filled_emb,lengths);
          filled_emb=0;
    }

#pragma acc wait

    if(want_total) {
        const unsigned int start_idx = task_p->start;
        const unsigned int stop_idx = task_p->stop;

        TFloat * const dm_stripes_buf = taskObj.dm_stripes.buf;
        const TFloat * const dm_stripes_total_buf = taskObj.dm_stripes_total.buf;

#pragma acc parallel loop collapse(2) present(dm_stripes_buf,dm_stripes_total_buf)
        for(unsigned int i = start_idx; i < stop_idx; i++)
            for(unsigned int j = 0; j < n_samples; j++) {
                unsigned int idx = (i-start_idx)*n_samples_r+j;
                dm_stripes_buf[idx]=dm_stripes_buf[idx]/dm_stripes_total_buf[idx];
                // taskObj.dm_stripes[i][j] = taskObj.dm_stripes[i][j] / taskObj.dm_stripes_total[i][j];
            }
        
    }

#pragma acc exit data delete(lengths[:max_emb])
#pragma acc exit data delete(embedded_proportions[:n_samples_r*max_emb])
    free(lengths);
    free(embedded_proportions);
}

void su::unifrac(biom &table,
                 BPTree &tree,
                 Method unifrac_method,
                 std::vector<double*> &dm_stripes,
                 std::vector<double*> &dm_stripes_total,
                 const su::task_parameters* task_p) {
    switch(unifrac_method) {
        case unweighted:
            unifracTT<UnifracUnweightedTask<double>,double>(           table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_normalized:
            unifracTT<UnifracNormalizedWeightedTask<double>,double>(   table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_unnormalized:
            unifracTT<UnifracUnnormalizedWeightedTask<double>,double>( table, tree, false, dm_stripes,dm_stripes_total,task_p);
            break;
        case generalized:
            unifracTT<UnifracGeneralizedTask<double>,double>(          table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case unweighted_fp32:
            unifracTT<UnifracUnweightedTask<float >,float>(            table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_normalized_fp32:
            unifracTT<UnifracNormalizedWeightedTask<float >,float>(    table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_unnormalized_fp32:
            unifracTT<UnifracUnnormalizedWeightedTask<float >,float>(  table, tree, false, dm_stripes,dm_stripes_total,task_p);
            break;
        case generalized_fp32:
            unifracTT<UnifracGeneralizedTask<float >,float>(           table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        default:
            fprintf(stderr, "Unknown unifrac task\n");
            exit(1);
            break;
    }
}


template<class TaskT, class TFloat>
void unifrac_vawTT(biom &table,
                          BPTree &tree,
                          const bool want_total,
                          std::vector<double*> &dm_stripes,
                          std::vector<double*> &dm_stripes_total,
                          const su::task_parameters* task_p) {
    int err;
#ifndef _OPENACC
    // processor affinity, when not using openacc
    err = bind_to_core(task_p->tid);
    if(err != 0) {
        fprintf(stderr, "Unable to bind thread %d to core: %d\n", task_p->tid, err);
        exit(EXIT_FAILURE);
    }
#endif

    if(table.n_samples != task_p->n_samples) {
        fprintf(stderr, "Task and table n_samples not equal\n");
        exit(EXIT_FAILURE);
    }
    const unsigned int n_samples = task_p->n_samples;
    const uint64_t  n_samples_r = ((n_samples + UNIFRAC_BLOCK-1)/UNIFRAC_BLOCK)*UNIFRAC_BLOCK; // round up

    PropStack propstack(table.n_samples);
    PropStack countstack(table.n_samples);

    const unsigned int max_emb = 128;

    uint32_t node;
    double *node_proportions;
    double *node_counts;
    TFloat *embedded_proportions;
    TFloat *embedded_counts;
    TFloat *sample_total_counts;

    initialize_embedded<TFloat>(embedded_proportions, max_emb, task_p);
    initialize_embedded<TFloat>(embedded_counts, max_emb, task_p);
    initialize_sample_counts<TFloat>(sample_total_counts, task_p, table);
    initialize_stripes(std::ref(dm_stripes), std::ref(dm_stripes_total), want_total, task_p);

    TaskT taskObj(std::ref(dm_stripes), std::ref(dm_stripes_total), embedded_proportions, embedded_counts, sample_total_counts, max_emb, task_p);

    TFloat *lengths = NULL;
    err = posix_memalign((void **)&lengths, 4096, sizeof(TFloat) * max_emb);
    if(err != 0) {
        fprintf(stderr, "posix_memalign(%d) failed: %d\n", sizeof(TFloat) * max_emb, err);
        exit(EXIT_FAILURE);
    }
#pragma acc enter data create(lengths[:max_emb])

    unsigned int filled_emb = 0;

    for(unsigned int k = 0; k < (tree.nparens / 2) - 1; k++) {
        node = tree.postorderselect(k);
        lengths[filled_emb] = tree.lengths[node];

        node_proportions = propstack.pop(node);
        node_counts = countstack.pop(node);

        set_proportions(node_proportions, tree, node, table, propstack);
        set_proportions(node_counts, tree, node, table, countstack, false);

        if(task_p->bypass_tips && tree.isleaf(node))
            continue;

        embed_proportions<TFloat>(embedded_proportions, node_proportions, filled_emb, task_p->n_samples);
        embed_proportions<TFloat>(embedded_counts, node_counts, filled_emb, task_p->n_samples);
        filled_emb++;

        if (filled_emb==max_emb) {
#pragma acc wait
#pragma acc update device(embedded_proportions[:n_samples_r*filled_emb],embedded_counts[:n_samples_r*filled_emb],lengths[:filled_emb])
          taskObj._run(filled_emb,lengths);
          filled_emb = 0;

          if(__builtin_expect(report_status[task_p->tid], false)) {
            sync_printf("tid:%d\tstart:%d\tstop:%d\tk:%d\ttotal:%d\n", task_p->tid, task_p->start, task_p->stop, k, (tree.nparens / 2) - 1);
            report_status[task_p->tid] = false;
          }
        }
    }

    if (filled_emb>0) {
#pragma acc wait
#pragma acc update device(embedded_proportions[:n_samples_r*filled_emb],embedded_counts[:n_samples_r*filled_emb],lengths[:filled_emb])
          taskObj._run(filled_emb,lengths);
          filled_emb = 0;
    }

#pragma acc wait
    if(want_total) {
        const unsigned int start_idx = task_p->start;
        const unsigned int stop_idx = task_p->stop;

        TFloat * const dm_stripes_buf = taskObj.dm_stripes.buf;
        const TFloat * const dm_stripes_total_buf = taskObj.dm_stripes_total.buf;

#pragma acc parallel loop collapse(2) present(dm_stripes_buf,dm_stripes_total_buf)
        for(unsigned int i = start_idx; i < stop_idx; i++)
            for(unsigned int j = 0; j < n_samples; j++) {
                unsigned int idx = (i-start_idx)*n_samples_r+j;
                dm_stripes_buf[idx]=dm_stripes_buf[idx]/dm_stripes_total_buf[idx];
                // taskObj.dm_stripes[i][j] = taskObj.dm_stripes[i][j] / taskObj.dm_stripes_total[i][j];
            }

    }


#pragma acc exit data delete(lengths[:max_emb])
#pragma acc exit data delete(embedded_proportions[:n_samples_r*max_emb],embedded_counts[:n_samples_r*max_emb],sample_total_counts[:n_samples_r])
    free(lengths);
    free(embedded_proportions);
    free(embedded_counts);
    free(sample_total_counts);
}

void su::unifrac_vaw(biom &table,
                     BPTree &tree,
                     Method unifrac_method,
                     std::vector<double*> &dm_stripes,
                     std::vector<double*> &dm_stripes_total,
                     const su::task_parameters* task_p) {
    switch(unifrac_method) {
        case unweighted:
            unifrac_vawTT<UnifracVawUnweightedTask<double>,double>(           table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_normalized:
            unifrac_vawTT<UnifracVawNormalizedWeightedTask<double>,double>(   table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_unnormalized:
            unifrac_vawTT<UnifracVawUnnormalizedWeightedTask<double>,double>( table, tree, false, dm_stripes,dm_stripes_total,task_p);
            break;
        case generalized:
            unifrac_vawTT<UnifracVawGeneralizedTask<double>,double>(          table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case unweighted_fp32:
            unifrac_vawTT<UnifracVawUnweightedTask<float >,float >(           table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_normalized_fp32:
            unifrac_vawTT<UnifracVawNormalizedWeightedTask<float >,float >(   table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        case weighted_unnormalized_fp32:
            unifrac_vawTT<UnifracVawUnnormalizedWeightedTask<float >,float >( table, tree, false, dm_stripes,dm_stripes_total,task_p);
            break;
        case generalized_fp32:
            unifrac_vawTT<UnifracVawGeneralizedTask<float >,float >(          table, tree, true,  dm_stripes,dm_stripes_total,task_p);
            break;
        default:
            fprintf(stderr, "Unknown unifrac task\n");
            exit(1);
            break;
    }
}

void su::set_proportions(double* props,
                         BPTree &tree,
                         uint32_t node,
                         biom &table,
                         PropStack &ps,
                         bool normalize) {
    if(tree.isleaf(node)) {
       table.get_obs_data(tree.names[node], props);
       for(unsigned int i = 0; i < table.n_samples; i++) {
           props[i] = props[i];
           if(normalize)
               props[i] /= table.sample_counts[i];
       }

    } else {
        unsigned int current = tree.leftchild(node);
        unsigned int right = tree.rightchild(node);
        double *vec;

        for(unsigned int i = 0; i < table.n_samples; i++)
            props[i] = 0;

        while(current <= right && current != 0) {
            vec = ps.get(current);  // pull from prop map
            ps.push(current);  // remove from prop map, place back on stack

            for(unsigned int i = 0; i < table.n_samples; i++)
                props[i] = props[i] + vec[i];

            current = tree.rightsibling(current);
        }
    }
}

std::vector<double*> su::make_strides(unsigned int n_samples) {
    uint32_t n_rotations = (n_samples + 1) / 2;
    std::vector<double*> dm_stripes(n_rotations);

    int err = 0;
    for(unsigned int i = 0; i < n_rotations; i++) {
        double* tmp;
        err = posix_memalign((void **)&tmp, 32, sizeof(double) * n_samples);
        if(tmp == NULL || err != 0) {
            fprintf(stderr, "Failed to allocate %zd bytes, err %d; [%s]:%d\n",
                    sizeof(double) * n_samples, err,  __FILE__, __LINE__);
            exit(EXIT_FAILURE);
        }
        for(unsigned int j = 0; j < n_samples; j++)
            tmp[j] = 0.0;
        dm_stripes[i] = tmp;
    }
    return dm_stripes;
}


void su::process_stripes(biom &table,
                         BPTree &tree_sheared,
                         Method method,
                         bool variance_adjust,
                         std::vector<double*> &dm_stripes,
                         std::vector<double*> &dm_stripes_total,
                         std::vector<std::thread> &threads,
                         std::vector<su::task_parameters> &tasks) {

    // register a signal handler so we can ask the master thread for its
    // progress
    if (signal(SIGUSR1, sig_handler) == SIG_ERR)
        fprintf(stderr, "Can't catch SIGUSR1\n");

    report_status = (bool*)calloc(sizeof(bool), CPU_SETSIZE);
    pthread_mutex_init(&printf_mutex, NULL);

#ifdef _OPENACC
    // cannot use threading with openacc
    for(unsigned int tid = 0; tid < threads.size(); tid++) {
        if(variance_adjust)
            su::unifrac_vaw(
                                       std::ref(table),
                                       std::ref(tree_sheared),
                                       method,
                                       std::ref(dm_stripes),
                                       std::ref(dm_stripes_total),
                                       &tasks[tid]);
        else
            su::unifrac(
                                       std::ref(table),
                                       std::ref(tree_sheared),
                                       method,
                                       std::ref(dm_stripes),
                                       std::ref(dm_stripes_total),
                                       &tasks[tid]);
    }
#else
    for(unsigned int tid = 0; tid < threads.size(); tid++) {
        if(variance_adjust)
            threads[tid] = std::thread(su::unifrac_vaw,
                                       std::ref(table),
                                       std::ref(tree_sheared),
                                       method,
                                       std::ref(dm_stripes),
                                       std::ref(dm_stripes_total),
                                       &tasks[tid]);
        else
            threads[tid] = std::thread(su::unifrac,
                                       std::ref(table),
                                       std::ref(tree_sheared),
                                       method,
                                       std::ref(dm_stripes),
                                       std::ref(dm_stripes_total),
                                       &tasks[tid]);
    }

    for(unsigned int tid = 0; tid < threads.size(); tid++) {
        threads[tid].join();
    }
#endif

    if(report_status != NULL) {
        pthread_mutex_destroy(&printf_mutex);
        free(report_status);
    }
}
