#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mkl.h>
#include <time.h>
#include <sys/time.h>

int cols;
double val, x;
char* line;
int max_line_len = 1024*10;
double cgTol = 1e-20;

void printArray(double *arr, int n){
  int i;
  for (i = 0; i < n; ++i)
  {
    printf("%.10f\n",arr[i] );
  }

}

void getMaxMin(int *colidx, double *vals, int d, int nnz, int m,  double *max, double *min, int *nnzArray){
  int i;
  for (i = 0; i < d; ++i)
  {
    max[i]=-1e9;
    min[i]=1e9;
        // what if there are 0's in the column but we don't
        // see it since it is sparse?!
        // we have to consider 0's too
    if(nnzArray[i]<m){
      max[i]=0;
      min[i]=0;
    }
  }

  for (i = 0; i < nnz; ++i)
  {
    int index = colidx[i]-1;
    double val = vals[i];
    if (max[index]<val)
    {
      max[index]= val;
    }
    if (min[index]>val)
    {
      min[index]=val;
    }

  }
  for (i = 0; i < d; ++i)
  {
    if(max[i]< -1e8)
      max[i]=0.0;
    if(min[i]>1e8)
      min[i]=0.0;
  }
}

// normalize the dataset between 0 and 1
void normalize(int *colidx, double *vals, int d, int nnz, double* max, double *min){
  int i;
  for (i = 0; i < nnz; ++i)
  {
    int index = colidx[i]-1;
    if (max[index]-min[index]!=0)
    {
      vals[i] = (vals[i]-min[index])/(max[index]-min[index]);
    }else{
     vals[i] = (vals[i]-min[index]); 
   }

 }

}



//  s=x-y
void xpby(double *x, double *y, double *s, double b, int d){
  memset(s,0,sizeof(double)*d);
  cblas_daxpy (d, -b, y, 1, s, 1);
  cblas_daxpy (d, 1, x, 1, s, 1);
}


// gradient function for logistic regression
void gradfun(double *x,
  double *vals,
  int *colidx,
  int *pointerB,
  int *pointerE,
  char *descr ,
  double *y ,
  int d,
  int m,
  int m_local,
  double lambda,
  double *g){

  double alpha = 1.0;
  double nalpha = -1.0;
  double zero =0.0;
  int inc = 1;
  int i;

  double *Ax = (double*) malloc (m_local*sizeof(double));
  double *v = (double*) malloc (m_local*sizeof(double));
  double *e = (double*) malloc (m_local*sizeof(double));
  double *ep1 = (double*) malloc (m_local*sizeof(double));

    // op: Ax = A*x
  char trans ='N';
  mkl_dcsrmv (&trans
    , &m_local
    , &d
    , &alpha
    , descr
    , vals
    , colidx
    , pointerB
    , pointerE
    , x
    , &zero
    , Ax );

    // op: v = Ax.*y
  vdMul( m_local, Ax, y, v );

    // op: e = exp(v)
  vdExp( m_local, v, e );

    // op: ep1 = 1/(1+e)
  for (i = 0; i < m_local; ++i)
  {
    ep1[i] = 1.0/(1 + e[i]);
  }
    //op: v = y.*ep1
  vdMul( m_local, y, ep1, v );

    // op: g = -A*vs
  char transa ='T';
  mkl_dcsrmv (&transa 
    , &m_local
    , &d
    , &nalpha
    , descr
    , vals
    , colidx
    , pointerB
    , pointerE
    , v
    , &zero
    , g );

  double a = lambda;
  double b = 1.0/m_local;

    //double b = 1.0/m;
  cblas_daxpby (d, a, x, inc, b, g, inc);

  free(v);
  free(e);
  free(ep1);
  free(Ax);

}

// part of GIANT algorithm for getting "A" in their paper
void getAmatrix(double *x,
  double *vals,
  int *colidx,
  int *pointerB,
  int *pointerE,
  char *descr ,
  double *y ,
  int d,
  int nnz_local,
  int m_local,
  double *g){

  double alpha = 1.0;
  double nalpha = -1.0;
  double zero =0.0;
  int inc = 1;
  int i, j;

  double *Ax = (double*) malloc (m_local*sizeof(double));
  double *v = (double*) malloc (m_local*sizeof(double));
  double *e = (double*) malloc (m_local*sizeof(double));
  double *ep1 = (double*) malloc (m_local*sizeof(double));
  double *sqrtE = (double*) malloc (m_local*sizeof(double));

    // op: Ax = A*x
  char trans ='N';
  mkl_dcsrmv (&trans
    , &m_local
    , &d
    , &alpha
    , descr
    , vals
    , colidx
    , pointerB
    , pointerE
    , x
    , &zero
    , Ax );
  
    // op: v = Ax.*y
  vdMul( m_local, Ax, y, v );

    // op: e = exp(v) and sqrtE = sqrt(e)
  vdExp( m_local, v, e );
  vdSqrt(m_local, e,sqrtE);

    // op: ep1 = 1/(1+e)
  for (i = 0; i < m_local; ++i)
  {
    ep1[i] = sqrtE[i]/(1 + e[i]);
  }

    //op: v = y.*ep1
  vdMul( m_local, y, ep1, v );


    //op: scale rows of input data by elemnts of v
    j = 0; // current row
    for (i = 0; i < nnz_local; ++i)
    {
      if(i < pointerE[j]-1)
        g[i] = vals[i]* v[j];
      else{
        j++;
        g[i] = vals[i]* v[j];
      }
    }


    free(v);
    free(e);
    free(ep1);
    free(Ax);
    free(sqrtE);

  }


// GIANT local solver which is Conjugate Gradient
  void local_solver(double *x,
    double *vals,
    int *colidx,
    int *rowidx,
    char *descr,
    double *y ,
    int d,
    int m,
    int m_local,
    int nnz_local,
    double *gsum,
    double lambda,
    double *xs,
    double *b,
    double *r,
    double *pcg,
    double *ap){



    // auxilary varaibles
    int i,j;
    int inc = 1;

    // mkl parameters
    double alphaMKL = 1.0;
    double nalpha = -1.0;
    double zero =0.0;
    char trans ='N';
    char transA ='T';


    //////////////////////
    ////////////////////
    // implementing CG
    //Solve (1/s * At * At' + lam* I) * xs = gsum

    // CG parameters:
    int q = 20; // maximum number of iterations for local problems
    int s = m_local; // number of local samples, in the paper > "s"
    
    double *temp1 = (double*) malloc (m_local*sizeof(double));
    double *temp2 = (double*) malloc (m_local*sizeof(double));

    memset(b, 0, sizeof(double)*d); 
    memset(r, 0, sizeof(double)*d);
    memset(pcg, 0, sizeof(double)*d);
    memset(ap, 0, sizeof(double)*d);


    double lam = s * lambda;
    for (i = 0; i < d; ++i)
    {
      b[i] = s * gsum[i];
    }

    
    memset(xs, 0, sizeof(double)*d); // "w" = xs

    //HessianProduct
    mkl_dcsrmv (&trans
      , &m_local
      , &d
      , &alphaMKL
      , descr
      , vals
      , colidx
      , rowidx
      , rowidx+1
      , xs
      , &zero
      , temp1 );

    mkl_dcsrmv (&transA
      , &m_local
      , &d
      , &alphaMKL
      , descr
      , vals
      , colidx
      , rowidx
      , rowidx+1
      , temp1
      , &zero
      , r );



    cblas_daxpby (d, -lam, xs, 1, -1, r, 1);

    cblas_daxpby (d, 1, b, 1, 1, r, 1);


    memcpy (pcg, r, d*sizeof(double)); //p = r
    double rsold = ddot(&d, r, &inc, r, &inc);

    double rsnew = 0 ; 
    double alpha = 0 ;
    double rssqrt = 0 ;
    double pap = 0 ;

    double tol =cgTol * sqrt(ddot(&d, b, &inc, b, &inc));

    for (i = 0; i < q; ++i)
    {

        //HessianProduct a * (a.t * p)
      mkl_dcsrmv (&trans
        , &m_local
        , &d
        , &alphaMKL
        , descr
        , vals
        , colidx
        , rowidx
        , rowidx+1
        , pcg
        , &zero
        , temp2 );

      mkl_dcsrmv (&transA
        , &m_local
        , &d
        , &alphaMKL
        , descr
        , vals
        , colidx
        , rowidx
        , rowidx+1
        , temp2
        , &zero
        , ap );


      cblas_daxpby (d, lam, pcg, 1, 1, ap, 1);
      pap = 0;
      for (j = 0; j < d; ++j)
      {
        pap += ap[j] * pcg[j];
      }
      alpha = rsold/pap;

      cblas_daxpby (d, alpha, pcg, 1, 1, xs, 1);
      cblas_daxpby (d, -alpha, ap, 1, 1, r, 1);

      rsnew = ddot(&d, r, &inc, r, &inc);
      rssqrt = sqrt(rsnew);
      if(rssqrt<tol){
        break;
      }

      for (j = 0; j < d; ++j)
      {
        pcg[j] = (rsnew/rsold) * pcg[j] + r[j];
      }


      rsold = rsnew ;


    }

    cgTol = cgTol * 0.5;
    free(temp1);
    free(temp2);

  }

// objective function for logistic regression
  double objective_fun(double *x,
    double *vals,
    int *colidx,
    int *rowidx,
    char *descr, double *y , int d, int m, double lambda){

    double alpha = 1.0;
    double nalpha = -1.0;
    double zero =0.0;
    int inc = 1;
    int i;

    double *nAx = (double*) malloc (m*sizeof(double));
    double *v = (double*) malloc (m*sizeof(double));
    double *e = (double*) malloc (m*sizeof(double));

    char transa ='N';
    mkl_dcsrmv (&transa 
      , &m
      , &d
      , &nalpha
      , descr
      , vals
      , colidx
      , rowidx
      , rowidx+1
      , x
      , &zero
      , nAx );

    vdMul( m, nAx, y, v );
    vdExp( m, v, e );

    
    // op: v = ln(e+1)
    vdLog1p( m, e, v );

    double sum = 0;
    for (i = 0; i < m; ++i)
    {
      sum+= v[i];
    }
    sum = sum/m;
    sum += lambda/2*ddot(&d, x, &inc, x, &inc);


    free(v);
    free(e);
    free(nAx);

    return sum;
  }


// helper function to read file
  static char* readline(FILE *input)
  {
    int len;
    if(fgets(line,max_line_len,input) == NULL)
      return NULL;

    while(strrchr(line,'\n') == NULL)
    {
      max_line_len *= 2;
      line = (char *) realloc(line,max_line_len);
      len = (int) strlen(line);
      if(fgets(line+len,max_line_len-len,input) == NULL)
        break;
    }
    return line;
  }

// helper function to read dataset
  void readgg(char* fname, int *rowidx,int *colidx,
    double *vals,double *y , int *nnz_local, int *m_local, int* nnzArray)
  {

    int i;
    FILE * file;
    file = fopen(fname, "r");
    printf("%s\n",fname );


    if(file == NULL){
      printf("File not found!\n");
      return;
    }

    line= (char*) malloc(max_line_len*sizeof(char));

    int count_rowidx=0,count_colidx=0;;
    rowidx[count_rowidx]=1;
    i = 0;
    
    
    while (1)
    {
      if(readline(file)==NULL){
        break;
      }

      char *label, *value, *index, *endptr;
      label = strtok(line," \t\n");
      x = strtod(label, &endptr);
      while(1)
      {
        index = strtok(NULL,":");

        value = strtok(NULL," \t");

        if(value == NULL){
          break;
        }

        cols = (int) strtol(index,&endptr,10);
        colidx[count_colidx]=cols;
        nnzArray[cols-1]++;

        val =  strtod(value, &endptr);
        vals[count_colidx]=val;
        count_colidx++;
        i++;
      }
      count_rowidx++;
      rowidx[count_rowidx]=i+1;

      y[count_rowidx-1]=x;
    }
    *nnz_local = count_colidx;
    *m_local = count_rowidx;

    fclose(file);
  }


  int main(int argc, char** argv) {

    // Initialize the MPI environment
    MPI_Init(&argc,&argv);
    int world_size;
    int rank;
    struct timeval start,end;

    // Get the number of processes and rank
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);


    // Print a hello world message
    printf("rank %d: started\n",
     rank);
    if(world_size<1){
      printf("%s\n", "There should be at least one processors!");
      MPI_Finalize();
      return 0;
    }



// INPUT
    if(argc<8){
      printf("Input Format: pathname #rows #nnz #cols #iterations lambda freq\n");
      MPI_Finalize();
      return 0;
    }
    char* pathName = argv[1];
    int m = atoi(argv[2]);
    int nnz = atoi(argv[3]);
    int d = atoi(argv[4]);
    int Iter = atoi(argv[5]);
    double lambda = atof(argv[6]);
    
    int freq = atoi(argv[7]);
    int seed = 42;

    if(rank==0){
      printf("lambda is %f\n",lambda );
      printf("Iter is %d\n",Iter );
    }

    // TODO: move to the top
    double one = 1.0;
    MKL_INT inc = 1;
    double zero = 0.0;
    char trans = 'N';
    int cumX_size = (Iter)/freq +20;

    double *cumX = (double *) malloc((cumX_size*d)*sizeof(double));
    long *times = (long*) malloc((cumX_size)*sizeof(long));

    int *rowidx,*colidx, *nnzArray, nnz_local, m_local; // m_local: number of samples for each processor
    double *vals,*y, *valsScaled;
    char *descrA;
    int *rowidxFull,*colidxFull, nnz_full, m_full; 
    double *valsFull,*yFull;

    rowidx=(int *)malloc((m+1)*sizeof(int));
    colidx=(int *)malloc(nnz*sizeof(int));
    vals=(double *)malloc(nnz*sizeof(double));
    y=(double *)malloc(m*sizeof(double));

    // just for rank 0
    if(rank == 0){
      rowidxFull=(int *)malloc((m+1)*sizeof(int));
      colidxFull=(int *)malloc(nnz*sizeof(int));
      valsFull=(double *)malloc(nnz*sizeof(double));
      yFull=(double *)malloc(m*sizeof(double));
    }
    

    nnzArray=(int *)malloc(d*sizeof(int));
    descrA=(char *)malloc(6*sizeof(char));
    memset(nnzArray,0,d*sizeof(int));

    
    // Descriptor of main sparse matrix properties
    descrA[0]='G';
    descrA[3]='F';

    // local gradients : g
    // reduced gradient on root: gsum
    double *g = (double*) malloc (d*sizeof(double));
    double *gsum = (double*) malloc (d*sizeof(double));
    memset(g, 0, sizeof(double)*d);
    memset(gsum, 0, sizeof(double)*d);

    // CG variables
    double *b = (double*) malloc (d*sizeof(double)); // we use "b" as right hand side
    double *r = (double*) malloc (d*sizeof(double));
    double *pcg = (double*) malloc (d*sizeof(double));
    double *ap = (double*) malloc (d*sizeof(double));

    //Initialization
    double *x = (double*) malloc (d*sizeof(double));
    double *p = (double*) malloc (d*sizeof(double));
    memset(x, 0, sizeof(double)*d);

    // max and min of the dataset for normalization    
    double *max = (double *) malloc(d*sizeof(double));
    double *min = (double *) malloc(d*sizeof(double));
    int i;
    for ( i = 0; i < d; ++i)
    {
      max[i]=-1e9;
      min[i]=1e9;
    }
    double *maxAll = (double *) malloc(d*sizeof(double));
    double *minAll = (double *) malloc(d*sizeof(double));


    char pathBuff[1000];
    size_t destination_size = sizeof (pathBuff);
    strncpy(pathBuff, pathName, destination_size);
    pathBuff[destination_size - 1] = '\0';

    char buf[100];
    strcat(pathBuff, "-");
    sprintf(buf, "%d", rank);
    strcat(pathBuff,buf);

    // read the dataset
    readgg(pathBuff, rowidx, colidx, vals, y, &nnz_local, &m_local, nnzArray);
    valsScaled=(double *)malloc(nnz_local*sizeof(double));
    
        // normalizing the matrix between 0 and 1
        // first we get the max and min of local dataset
        // then we reduce it to get the overall max and min

    getMaxMin(colidx, vals, d, nnz_local, m_local, max, min,nnzArray);

    
    // get the maximum and minimum over all processors
    MPI_Allreduce(min, minAll, d, MPI_DOUBLE, MPI_MIN,
     MPI_COMM_WORLD);
    MPI_Allreduce(max, maxAll, d, MPI_DOUBLE, MPI_MAX,
     MPI_COMM_WORLD);

    //normalize the dataset using maxall and minall
    // which contain the max and min for each column

    normalize(colidx, vals, d, nnz_local, maxAll, minAll);

        // initial gradients 
    gradfun(x , vals, colidx, rowidx, rowidx+1, descrA, y, d ,m, m_local,lambda, g);
    

    if(rank == 0 ){

        // just for objective calculation
        // we don't need this for performance evaluation:
      readgg(pathName, rowidxFull, colidxFull, valsFull, yFull, &nnz_full, &m_full,nnzArray);
      normalize(colidxFull, valsFull, d, nnz_full, maxAll, minAll);

    }

    MPI_Allreduce(g, gsum, d, MPI_DOUBLE, MPI_SUM,
     MPI_COMM_WORLD);

    // new initial condition: which is one step gradeint descent
    double neta= -0.0001/(world_size);
    cblas_daxpy (d, neta, gsum, 1, x, 1);
    
    // updated x's 
    double *xs =(double*) malloc (d*sizeof(double));

    //start timing
    if(rank ==0){
      memcpy (cumX, x, d*sizeof(double)); 
      gettimeofday(&start,NULL);
      times[0] = 0;
    }
    // starting the algorithm
    int it = 0;
    while(it < Iter){

        // compute local gradients
      gradfun(x , vals, colidx, rowidx, rowidx+1, descrA, y, d ,m, m_local,lambda, g);
      MPI_Allreduce(g, gsum, d, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

      // average the gradeints:
      cblas_daxpby (d, 0, g, 1, 1.0/world_size, gsum, 1);

      // get matrix A(local Hessian) as indicated in the paper
      getAmatrix(x, vals, colidx, rowidx, rowidx+1, descrA, y, d, nnz_local, m_local, valsScaled);

      local_solver(x, valsScaled,colidx,rowidx, descrA, y, d, m, m_local, nnz_local, gsum,lambda, xs, b, r, pcg, ap);

      MPI_Allreduce(xs, p, d, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

        // average the x's:
      cblas_daxpby (d, -1.0/world_size, p, 1, 1, x, 1);

      if(it%freq == 0 ){
        gettimeofday(&end,NULL);
        long seconds = end.tv_sec - start.tv_sec;
        times[it/freq +1] = (seconds*1000)+(end.tv_usec - start.tv_usec)/1000;
        memcpy (cumX+(it/freq +1)*d, x, d*sizeof(double)); 
      }
      if(rank == 0){
        printf("Iteration %d is finished.\n", it);
      }
      it++;
    }
    

    free(xs);
    free(x);
    free(min);
    free(max);
    free(g);
    free(gsum);
    free(maxAll);
    free(minAll);
    free(p);

    free(b);
    free(r);
    free(pcg);
    free(ap);


    // Finalize the MPI environment.
    MPI_Finalize();

    // the next 2 lines are for computing the objective function
    // to report later
    
    double *gval =(double*) malloc (d*sizeof(double));
    if(rank == 0){
      for (i = 0; i < (Iter-1)/freq; ++i)
      {
        double obj_value = objective_fun(cumX+i*d, valsFull,colidxFull,rowidxFull, descrA , yFull , d, m, lambda);
        gradfun(cumX+i*d , valsFull,colidxFull,rowidxFull, rowidxFull+1, descrA, yFull, d ,m,  m_full,lambda, gval);
        double grad_value = ddot(&d, gval, &inc, gval, &inc);
        printf(" %ld , %.8f,%.8f \n", times[i], obj_value,grad_value);
      }
    }
    return 0;
  }