#include "simgrid/actor.h"
#include "simgrid/engine.h"
#include "simgrid/host.h"
#include "simgrid/mailbox.h"
#include "simgrid/mutex.h"
#include "simgrid/cond.h"
#include "xbt/log.h"
#include "xbt/str.h"
#include "xbt/sysdep.h"
#include "rand.h"
#include <string.h>


#define NUM_SERVERS	100
#define MFLOPS_BASE     (1000*1000*1000)  // para el cálculo del tiempo de servicio de cada tarea

#define NUM_CLIENTS	1
#define NUM_DISPATCHERS	1
#define NUM_TASKS	100000		// número de tareas a generar


#define SERVICE_RATE    1.0     // Mu = 1  service time = 1 / 1; tasa de servicio de cada servidor
double ARRIVAL_RATE;		// tasa de llegada, como argumento del programa

// Políticas de enrutado y planificación de cola (configurables por CLI)
typedef enum { DISPATCH_RANDOM = 0, DISPATCH_SQF = 1, DISPATCH_RR = 2, DISPATCH_TWO_RANDOM = 3, DISPATCH_TWO_RR_RANDOM = 4 } dispatch_policy_t;
typedef enum { QUEUE_FCFS = 0, QUEUE_SJF = 1, QUEUE_LJF = 2 } queue_policy_t;

static dispatch_policy_t DISPATCH_POLICY = DISPATCH_RANDOM; // por defecto
static queue_policy_t QUEUE_POLICY = QUEUE_FCFS;            // por defecto
static int rr_next_server = 0;                              // para round-robin


// variables para gestionar la cola de tareas en cada servidor
xbt_dynar_t 	client_requests[NUM_SERVERS] ;   // cola de peticiones en cada servidor, array dinamico de SimGrid
sg_mutex_t 	mutex[NUM_SERVERS];
sg_cond_t  	cond[NUM_SERVERS];
int 		EmptyQueue[NUM_SERVERS];	// indicacion de fin de cola en cada servidor


// variables para estadísticas
int 	Nqueue[NUM_SERVERS];          	// elementos en la cola de cada servidor esperando a ser servidos
double 	Navgqueue[NUM_SERVERS];         // tamanio medio de la cola de cada servidor

int 	Nsystem[NUM_SERVERS];		// número de tareas en cada servidor (esperando y siendo atendidas)
double 	Navgsystem[NUM_SERVERS];	// número medio de tareas por servidor (esperando y siendo atendidas)


double tiempoMedioServicio[NUM_TASKS];    // tiempo medio de servicio para cada tarea

// estrucrtura de la petcición que envía el cliente 
struct ClientRequest {
	int    n_task;      // número de tarea
	double t_arrival;   /* momento en el que llega la tarea (tiempo de creacion)*/
	double t_service;   /* tiempo de servicio asignado en FLOPS*/
};


// ordena dos elementos de tipo struct ClientRequest
// utilizado para poder ordenar la colaisutilizando la fucion xbt__dynar_sort
static int sort_function(const void *e1, const void *e2)
{
	struct ClientRequest *c1 = *(void **) e1;
	struct ClientRequest *c2 = *(void **) e2;

	if (c1->t_service == c2->t_service)
		return 0;

	else    if (c1->t_service < c2->t_service)
		return -1;
	else
		return 1;
}

// Orden descendente por t_service (para LJF)
static int sort_function_desc(const void *e1, const void *e2)
{
	struct ClientRequest *c1 = *(void **) e1;
	struct ClientRequest *c2 = *(void **) e2;

	if (c1->t_service == c2->t_service)
		return 0;
	else if (c1->t_service > c2->t_service)
		return -1; // mayor primero
	else
		return 1;
}


// Client function: genera las peticiones
int client(int argc, char *argv[])
{
  	char mailbox_name[256];
  	struct ClientRequest *req ;
  	double t_arrival;
	int my_c;
	double t;
	int k;

	my_c = atoi(argv[0]);  // identifiador del cliente
	
	for (k=0; k <NUM_TASKS; k++) {
		req = (struct ClientRequest *) xbt_malloc(sizeof(struct ClientRequest));

      		/* espera la llegada de una peticion */
		/* ARRIVAL_RATE peticiones por segundo, lamda = ARRIVAL_RATE5 */
      		t_arrival = exponential((double)ARRIVAL_RATE);

		sg_actor_sleep_for(t_arrival);

      		/* prepara la tarea para enviar */
      		req->t_arrival = simgrid_get_clock();                  // tiempo de llegada

		// tiempo de servicio asignada a la tarea
                // t medio de servicio = 1/SERVICE_RATE de seg
                // Como base se toma que en 1 seg se encutan MFLOPS_BASE flops
		t = exponential((double)SERVICE_RATE);

                req->t_service = MFLOPS_BASE * t;  // calculo del tiempo de servicio en funcion
						   // de la velocidad del host del servidor
		req->n_task = k;

		// ahora se la envía a un único dispather
		sprintf(mailbox_name, "d-%d", 0);
		sg_mailbox_t mailbox = sg_mailbox_by_name(mailbox_name);

		sg_mailbox_put(mailbox, req, 0);
	}

	// indicar fin de la ejecución
	req = (struct ClientRequest *) xbt_malloc(sizeof(struct ClientRequest));
	req->n_task = -1;
	sprintf(mailbox_name, "d-%d", 0);
        sg_mailbox_t mailbox = sg_mailbox_by_name(mailbox_name);
        sg_mailbox_put(mailbox, req, 0);

    	/* finalizar */
  	return 0;
}                               


// dispatcher function, recibe las peticiones de los clientes y las envía a los servidores
int dispatcher(int argc, char *argv[])
{
        struct ClientRequest *req;
	int my_d;
	char mailbox_dispatcher_name[64];
	int k = 0;
	int s = 0;

	my_d = atoi(argv[0]);

	sprintf(mailbox_dispatcher_name,  "d-%d", my_d);
        sg_mailbox_t mailbox_dispatcher = sg_mailbox_by_name(mailbox_dispatcher_name);

	while (1) {
		req = (struct ClientRequest *) sg_mailbox_get(mailbox_dispatcher);

		if (req->n_task == -1)
			break;

		 ////////////////////////////////////////////////////////////
				 // ahora viene el algoritmo concreto del dispatcher     
                // para el algoritmo aleatorio se puede utilizar la función uniform_int definida en rand.c
                // para el algoritmo SQF se puede consultar directamente el array Nsystem que almacena
                // el número de elementos en cada uno de los servidores. Se trata de buscar el servidor con 
                // el menor numero de elementos en la cola.

				// Política seleccionada por CLI
				switch (DISPATCH_POLICY) {
					case DISPATCH_RANDOM:
						// Elección aleatoria uniforme entre 0 y NUM_SERVERS-1
						s = uniform_int(0, NUM_SERVERS - 1);
						break;
					case DISPATCH_TWO_RANDOM: {
						int a = uniform_int(0, NUM_SERVERS - 1);
						int b = uniform_int(0, NUM_SERVERS - 1);

						if (NUM_SERVERS > 1) {
							while (a == b) {
								b = uniform_int(0, NUM_SERVERS - 1);
							}
						}

						// Comparar cargas
						if (Nsystem[a] <= Nsystem[b]) {
							s = a;
						} else {
							s = b;
						}
						
						break;
					}
					case DISPATCH_TWO_RR_RANDOM: {
						// Elegir el servidor a por turno
						int a = rr_next_server;
						
						// Actualizamos el contador
						rr_next_server = (rr_next_server + 1) % NUM_SERVERS;

						// Elegir el servidor b al azar
						int b = uniform_int(0, NUM_SERVERS - 1);

						if (NUM_SERVERS > 1) {
							while (b == a) {
								b = uniform_int(0, NUM_SERVERS - 1);
							}
						}
						
						// Comparar cargas
						if (Nsystem[a] <= Nsystem[b]) {
							s = a;
						} else {
							s = b;
						}

						break;
					}
					case DISPATCH_SQF: {
						// Selecciona el servidor con menor carga
						int best = 0;
						int best_val = Nsystem[0];
						for (int i = 1; i < NUM_SERVERS; i++) {
							if (Nsystem[i] < best_val) {
								best_val = Nsystem[i];
								best = i;
							}
						}
						s = best;
						break;
					}
					case DISPATCH_RR:
						s = rr_next_server;
						rr_next_server = (rr_next_server + 1) % NUM_SERVERS;
						break;
					default:
						s = uniform_int(0, NUM_SERVERS - 1);
				}


		char mailbox_server_name[64];
                sprintf(mailbox_server_name, "s-%d", s);
        	sg_mailbox_t mailbox_server = sg_mailbox_by_name(mailbox_server_name);

		sg_mailbox_put(mailbox_server, req, 0);

		k++;
        }

	// enviar el fin (req->n_task == -1) a todos los servidores
	for (int i = 0; i< NUM_SERVERS; i++) {
		char mailbox_server_name[64];
                sprintf(mailbox_server_name, "s-%d", i);
        	sg_mailbox_t mailbox_server = sg_mailbox_by_name(mailbox_server_name);
		sg_mailbox_put(mailbox_server, req, 0);
	}

	return 0;
}


/** server function  */
int server(int argc, char *argv[])
{
  	struct ClientRequest *req;
  	int res;
	int my_s;
	
	my_s = atoi(argv[0]);

	char mailbox_server_name[64];
	sprintf(mailbox_server_name, "s-%d", my_s);
  	sg_mailbox_t mailbox_server = sg_mailbox_by_name(mailbox_server_name);

  	while (1) {
		req = (struct ClientRequest *) sg_mailbox_get(mailbox_server);

		if (req->n_task == -1)
			break;
		
		// inserta la petición en la cola
    		sg_mutex_lock(mutex[my_s]);
    		Nqueue[my_s]++;   // un elemento mas en la cola 
    		Nsystem[my_s]++;  // un elemento mas en el sistema 

		//FCFS: push al final; 
		//SJF: push y ordenar ascendente; 
		//LJF: ordenar descendente;
		xbt_dynar_push(client_requests[my_s], (const char *)&req);
		if (QUEUE_POLICY == QUEUE_SJF) {
			xbt_dynar_sort(client_requests[my_s], sort_function);
		} else if (QUEUE_POLICY == QUEUE_LJF) {
			xbt_dynar_sort(client_requests[my_s], sort_function_desc);
		}

		sg_cond_notify_one(cond[my_s]);  // despierta al proceso server
    		sg_mutex_unlock(mutex[my_s]);

	}  

	// marca el fin
 	sg_mutex_lock(mutex[my_s]);
	EmptyQueue[my_s] = 1;
	sg_cond_notify_all(cond[my_s]);
    	sg_mutex_unlock(mutex[my_s]);

  	return 0;
}       


/** server function  */
int dispatcherServer(int argc, char *argv[])
{
	int res;
  	struct ClientRequest *req;
	double Nqueue_avg = 0.0;
	double Nsystem_avg = 0.0;
	double c;
	int n_tasks = 0;
	int my_s;

	my_s = atoi(argv[0]);

	while (1) {
    		sg_mutex_lock(mutex[my_s]);

		while ((Nqueue[my_s] ==  0)   && (EmptyQueue[my_s] == 0)) {
			sg_cond_wait(cond[my_s], mutex[my_s]);
		}

		if ((EmptyQueue[my_s] == 1) && (Nqueue[my_s] == 0)) {
			sg_mutex_unlock(mutex[my_s]);
			break;
		}
		// extrae un elemento de la cola
                xbt_dynar_shift(client_requests[my_s], (char *) &req);

		Nqueue[my_s]--;  // un elemento menos en la cola

		n_tasks ++;

		// calculo de estadisticas
		Navgqueue[my_s] = (Navgqueue[my_s] * (n_tasks-1) + Nqueue[my_s]) / n_tasks;
		Navgsystem[my_s] = (Navgsystem[my_s] * (n_tasks-1) + Nsystem[my_s]) / n_tasks;

    		sg_mutex_unlock(mutex[my_s]);

		// crea una tarea para su ejecución

		sg_actor_execute(req->t_service);

    		sg_mutex_lock(mutex[my_s]);
		Nsystem[my_s]--;  // un elemento menos en el sistema
    		sg_mutex_unlock(mutex[my_s]);

		c = simgrid_get_clock();  // tiempo de terminacion de la tarea
		tiempoMedioServicio[req->n_task] = c - (req->t_arrival);

		free(req);
	}
	return 0;
}


void test_all(char *file)
{
	int argc;
        char str[50];
        int i;

	simgrid_load_platform(file);


	// el proceso client es el que genera las peticiones
  	simgrid_register_function("client", client);

	// el proceso dispatcher es el que distribuye las peticiones que le llegan a los servidores
  	simgrid_register_function("dispatcher", dispatcher);

	// cada servidor tiene un proceso server que recibe las peticiones: server
	// y un proceso dispatcher que las ejecuta
  	simgrid_register_function("server", server);
  	simgrid_register_function("dispatcherServer", dispatcherServer);

	for (i=0; i < NUM_SERVERS; i++) {
                sprintf(str,"s-%d", i);
                argc = 1;
                char **argvc=xbt_new(char*,2);

                argvc[0] = bprintf("%d",i);
                argvc[1] = NULL;

		sg_actor_create(str, sg_host_by_name(str), server, argc, argvc);
        }

        for (i=0; i < NUM_SERVERS; i++) {
                sprintf(str,"s-%d", i);
                argc = 1;
                char **argvc=xbt_new(char*,2);

                argvc[0] = bprintf("%d",i);
                argvc[1] = NULL;

		sg_actor_create(str, sg_host_by_name(str), dispatcherServer,  argc, argvc);
        }

	 for (i=0; i < NUM_CLIENTS; i++) {
                sprintf(str,"c-%d", i);
                argc = 1;
                char **argvc=xbt_new(char*,2);

                argvc[0] = bprintf("%d",i);
                argvc[1] = NULL;

		sg_actor_create(str, sg_host_by_name(str), client, argc, argvc);
        }

	 for (i=0; i < NUM_DISPATCHERS; i++) {
                sprintf(str,"d-%d", i);
                argc = 1;
                char **argvc=xbt_new(char*,2);

                argvc[0] = bprintf("%d",i);
                argvc[1] = NULL;

		sg_actor_create(str, sg_host_by_name(str), dispatcher, argc, argvc);
	}

	return;
}


/** Main function */
int main(int argc, char *argv[])
{
	int i;

	double t_medio_servicio = 0.0;		// tiempo medio de servicio de cada tarea
        double q_medio = 0.0; 			// tamaño medio de la cola (esperando a ser servidos)
        double n_medio = 0.0;			// número medio de tareas en el sistema (esperando y ejecutando)

	 if (argc < 3) {
		printf("Usage: %s platform_file lambda [dispatcher_policy] [queue_policy]\n", argv[0]);
		printf("  dispatcher_policy: random | sqf | rr | two-random-choices | two-rr-random-choices (default: random)\n");
		printf("  queue_policy      : fcfs | sjf | ljf (default: fcfs)\n");
                exit(1);
        }

        seed((int) time(NULL));
	ARRIVAL_RATE = atof(argv[2]) *  NUM_SERVERS;

	// Parse optional policies
	if (argc >= 4 && argv[3]) {
		if (strcmp(argv[3], "random") == 0) DISPATCH_POLICY = DISPATCH_RANDOM;
		else if (strcmp(argv[3], "sqf") == 0) DISPATCH_POLICY = DISPATCH_SQF;
		else if (strcmp(argv[3], "rr") == 0) DISPATCH_POLICY = DISPATCH_RR;
		else if (strcmp(argv[3], "two-random-choices") == 0 || strcmp(argv[3], "two-random") == 0) DISPATCH_POLICY = DISPATCH_TWO_RANDOM;
		else if (strcmp(argv[3], "two-rr-random-choices") == 0 || strcmp(argv[3], "two-rr-random") == 0) DISPATCH_POLICY = DISPATCH_TWO_RR_RANDOM;
		else {
			printf("[warn] Unknown dispatcher_policy '%s', using 'random'\n", argv[3]);
			DISPATCH_POLICY = DISPATCH_RANDOM;
		}
	}

	if (argc >= 5 && argv[4]) {
		if (strcmp(argv[4], "fcfs") == 0) QUEUE_POLICY = QUEUE_FCFS;
		else if (strcmp(argv[4], "sjf") == 0) QUEUE_POLICY = QUEUE_SJF;
		else if (strcmp(argv[4], "ljf") == 0) QUEUE_POLICY = QUEUE_LJF;
		else {
			printf("[warn] Unknown queue_policy '%s', using 'fcfs'\n", argv[4]);
			QUEUE_POLICY = QUEUE_FCFS;
		}
	}

  	simgrid_init(&argc, argv);

	for (i = 0; i < NUM_SERVERS; i++) {
		Nqueue[i] =0;
		Nsystem[i] =0;
		EmptyQueue[i]=0;
  		mutex[i] = sg_mutex_init();
  		cond[i] = sg_cond_init();
		client_requests[i] = xbt_dynar_new(sizeof(struct ClientRequest *), NULL);
	}

	test_all(argv[1]);

	simgrid_run();
	
        for (i = 0; i < NUM_TASKS; i++){
                t_medio_servicio = t_medio_servicio + tiempoMedioServicio[i];
        }

        for (i = 0; i < NUM_SERVERS; i++){
                q_medio = q_medio + Navgqueue[i];
                n_medio = n_medio + Navgsystem[i];
        }

        t_medio_servicio = t_medio_servicio / (NUM_TASKS);
        q_medio = q_medio / (NUM_SERVERS);
        n_medio = n_medio / (NUM_SERVERS);

	printf("tiempoMedioServicio \t TamañoMediocola \t    TareasMediasEnElSistema  \t   tareas\n");
        printf("%g \t\t\t %g \t\t\t  %g  \t\t\t  %d \n", t_medio_servicio, q_medio,  n_medio, NUM_TASKS );

	for (i = 0; i < NUM_SERVERS; i++) {
		xbt_dynar_free(&client_requests[i]);
	}

	return 0;
}

