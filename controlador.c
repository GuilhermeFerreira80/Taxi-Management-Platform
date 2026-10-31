



#include "controlador.h"

#include <stdarg.h>   // para va_list em safe_printf()

/* Estruturas e sincronização */
static servico servs[MAX_SERV];         // Array estático que armazena todos os serviços
static int nserv = 0;                   // Contador de serviços registados
static int next_id = 1;                 // Gerador de IDs sequenciais
static int nveiculos = MAX_VEIC;        // Limite máximo de veículos simultâneos
static int veiculos_em_exec = 0;        // Contador de veículos a correr atualmente
static long simulated_time = 1;         // Tempo simulado do sistema (incrementa 1 por seg)
static long start_time_unix;            // Timestamp real de início (para referência)
static long total_km = 0;               // Estatística global de KMs
static volatile int terminar_flag = 0;  // Flag global para sinalizar paragem do sistema

static pthread_t admin_tid;             // ID da thread de administração
static pthread_mutex_t serv_mutex = PTHREAD_MUTEX_INITIALIZER;  // Protege o array 'servs' e contadores relacionados
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER; // Garante que prints não se misturam na consola

/* --- Estrutura de Verificacao de Clientes Ativos --- */
static utilizador_ligado utls_ligados[MAX_UTIL_LIGADOS];
static pthread_mutex_t utls_mutex = PTHREAD_MUTEX_INITIALIZER; // Protege a lista de utilizadores ligados
/* --------------------------------------------------- */

/* --- Verificacao de Progresso da Frota --- */
// Armazena o último progresso conhecido para cada ID de serviço
static int progresso_servico[MAX_SERV]; 
// PS:Este array usa o ID do serviço como índice
// Usar find_index_by_id para localizar o índice correto em servs[].
// mas para simplicidade usar o índice do serviço no array servs.
// No controlador.h, MAX_SERV é 200.
static pthread_mutex_t prog_mutex = PTHREAD_MUTEX_INITIALIZER; // Protege a leitura/escrita do progresso
/* ------------------------------------------ */

/* --- utilitários --- */
/* Função thread-safe para imprimir na consola do controlador */
void safe_printf(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&print_mutex);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
}

/* Handler para SIGINT (Ctrl+C) - define flag para sair do loop principal */
void sigint_handler(int s){ (void)s; terminar_flag = 1; }

/* cria o fifo do servidor */
/* Se já existir, verifica erro. Se não, cria com permissões 0666 */
void cria_fifo_server(){
    unlink(SERVER_FIFO);
    if(mkfifo(SERVER_FIFO, 0666) < 0){
        if (errno != EEXIST) { perror("Erro ao criar fifo do servidor"); exit(1); }
    }
}

/* envia mensagem para fifo do cliente (non-blocking) */
/* Abre o FIFO do cliente, escreve a mensagem e fecha. Non-blocking para não travar o controlador. */
void enviar_cliente(const char *fifo, const char *msg){
    int fd = open(fifo, O_WRONLY | O_NONBLOCK);
    if(fd<0) return;
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    close(fd);
}

/* encontra índice do serviço por id, sob mutex externo se necessário */
/* Retorna -1 se não encontrar */
int find_index_by_id(int id){
    for(int i=0;i<nserv;i++){
        if(servs[i].id == id) return i;
    }
    return -1;
}

/* listar serviços */
/* Itera sobre o array de serviços e imprime detalhes. Protegido por mutex. */
void listar_servicos(){
    pthread_mutex_lock(&serv_mutex);
    safe_printf("[SISTEMA] Serviços (total:%d):\n", nserv);
    for(int i=0;i<nserv;i++){
        safe_printf("[id:%d] user:%s hora:%d local:%s dist:%dkm estado:%d pid:%d\n",
               servs[i].id, servs[i].user, servs[i].hora, servs[i].local, servs[i].distancia, servs[i].estado, (int)servs[i].pid_veiculo);
    }
    pthread_mutex_unlock(&serv_mutex);
}

/* remover serviço por id */
/* Remove do array fazendo shift dos elementos seguintes. */
void remover_serv_por_id(int id){
    pthread_mutex_lock(&serv_mutex);
    for(int i=0;i<nserv;i++){
        if(servs[i].id==id){
            for(int j=i;j<nserv-1;j++) servs[j]=servs[j+1];
            nserv--;
            break;
        }
    }
    pthread_mutex_unlock(&serv_mutex);
}

/* cancelar serviço(s) */
/* Marca estado como CANCELADO e envia sinal SIGUSR1 ao veículo se estiver a correr */
void cancelar_serv_id(int id){
    pthread_mutex_lock(&serv_mutex);
    for(int i=0; i<nserv; i++){
        if( (id == 0 || servs[i].id == id) ){
            // ** APENAS CANCELAR SE ESTIVER AGENDADO OU EM CURSO (Estado 0 ou 1) **
            if(servs[i].estado == S_AGENDADO || servs[i].estado == S_EM_CURSO){
                servs[i].estado = S_CANCELADO;
                safe_printf("[SISTEMA] Serviço id:%d cancelado com sucesso.\n", servs[i].id);
                // Se estiver em curso, envia sinal
                if(servs[i].pid_veiculo > 0){
                    kill(servs[i].pid_veiculo, SIGUSR1); // Sinal específico para o veículo cancelar
                }
            }
            if(id != 0 && servs[i].id == id) break; // Se for ID específico, para
        }
    }
    pthread_mutex_unlock(&serv_mutex);
}
/* imprime utilizadores / resumo */
/* Lista simples de utilizadores e estados */
void imprime_utilizadores(){
    pthread_mutex_lock(&serv_mutex);
    safe_printf("[SISTEMA] Lista de serviços por utilizador:\n");
    for(int i=0;i<nserv;i++){
        safe_printf("[id:%d] %s estado:%d\n", servs[i].id, servs[i].user, servs[i].estado);
    }
    pthread_mutex_unlock(&serv_mutex);
}

/* imprime frota em curso */
/* Mostra apenas veículos em movimento e o seu progresso (lido da thread de telemetria) */
void imprime_frota(){
    pthread_mutex_lock(&serv_mutex);
    pthread_mutex_lock(&prog_mutex); // Bloqueia o progresso também
    
    safe_printf("[SISTEMA] Frota em execução (ID | User | Progresso):\n");
    for(int i=0;i<nserv;i++){
        if(servs[i].estado==S_EM_CURSO){
            int perc = progresso_servico[i]; // Lê o progresso pelo índice 'i'
            safe_printf("[id:%d] user:%s | progresso:%d%% | pid:%d\n", 
                        servs[i].id, servs[i].user, perc, (int)servs[i].pid_veiculo);
        }
    }
    
    pthread_mutex_unlock(&prog_mutex);
    pthread_mutex_unlock(&serv_mutex);
}

/* atualizar total km (protegido) */
/* Chamado quando um veículo termina e reporta KMs */
void atualizar_total_km(int km){
    pthread_mutex_lock(&serv_mutex);
    total_km += km;
    pthread_mutex_unlock(&serv_mutex);
}

/* trata comandos administrativos (chamada pela thread admin) */
/* Processa comandos digitados no terminal do controlador */
void trata_input_admin(char *line){
    char cmd[64];
    if(sscanf(line, "%63s", cmd)!=1) return;
    if(strcmp(cmd,"listar")==0) listar_servicos();
    else if(strcmp(cmd,"utiliz")==0) imprime_utilizadores();
    else if(strcmp(cmd,"frota")==0) imprime_frota();
    else if(strcmp(cmd,"cancelar")==0){
        int id; if(sscanf(line+8, "%d", &id)==1) cancelar_serv_id(id);
        else safe_printf("[SISTEMA] Uso: cancelar <id>\n");
    }
    else if(strcmp(cmd,"km")==0){
        pthread_mutex_lock(&serv_mutex);
        safe_printf("[SISTEMA] Total de Km percorridos: %ldkm\n", total_km);
        pthread_mutex_unlock(&serv_mutex);
    }
    else if(strcmp(cmd,"hora")==0){
        pthread_mutex_lock(&serv_mutex);
        safe_printf("[SISTEMA] Hora simulada: %lds\n", simulated_time);
        pthread_mutex_unlock(&serv_mutex);
    }
    else if(strcmp(cmd,"terminar")==0){ 
        // Lógica de terminação do ADMIN (correta para o controlador)
        cancelar_serv_id(0); // Cancela todos os serviços
        terminar_flag = 1;  // Define a flag global para sair do loop principal
    }
    else safe_printf("[SISTEMA] Comando desconhecido\n");
}

/* processa pedido textual vindo do fifo */
/* Analisa strings como "REGISTER ...", "AGENDAR ...", etc. */
void processa_pedido(char *msg){
    char cmd[64];
    if(sscanf(msg, "%63s", cmd)!=1) return;
    
    // Em controlador.c, na função processa_pedido (bloco REGISTER):
    if(strcmp(cmd,"REGISTER")==0){
        char user[64], cf[128];
        if(sscanf(msg+9, "%63s %127s", user, cf)==2){
            int idx = -1;
            
            pthread_mutex_lock(&utls_mutex);
            
            // Tenta encontrar um slot ativo ou vazio
            for(int i=0; i<MAX_UTIL_LIGADOS; i++) {
                // Cheka se o usuário já está logado (se for o caso, pode ser um registo duplicado)
                if (utls_ligados[i].ativo == 1 && strcmp(utls_ligados[i].client_fifo, cf) == 0) {
                     idx = i; // Encontrado, mas já ativo.
                     break;
                }
                // Encontra o primeiro slot vazio
                if (utls_ligados[i].ativo == 0 && idx == -1) {
                    idx = i; 
                }
            }
            
            if(idx == -1){ // Se não achou slot vazio e nem estava logado
                pthread_mutex_unlock(&utls_mutex);
                enviar_cliente(cf, "ERRO maximo usuários atingidos");
            } else {
                // REGISTA O CLIENTE NA NOVA ESTRUTURA
                // PS:user não precisa ser armazenado, apenas o client_fifo para desligar
                strncpy(utls_ligados[idx].client_fifo, cf, sizeof(utls_ligados[idx].client_fifo));
                utls_ligados[idx].ativo = 1;
                
                pthread_mutex_unlock(&utls_mutex);
                enviar_cliente(cf, "OK registado");
            }
        }
    } else if(strcmp(cmd,"AGENDAR")==0){
        char user[64], local[64], cf[128];
        int hora, dist;
        /* formato: AGENDAR <username> <hora> <local> <distancia> <client_fifo> */
        if(sscanf(msg+8, "%63s %d %63s %d %127s", user, &hora, local, &dist, cf) == 5){
            
            // --- LÓGICA DE AGENDAMENTO ---
            pthread_mutex_lock(&serv_mutex);
            
            if(nserv >= MAX_SERV){ 
                pthread_mutex_unlock(&serv_mutex); 
                enviar_cliente(cf, "ERRO maximo serviços atingidos"); 
                return; 
            }
            
            servico s;
            s.id = next_id++;
            strncpy(s.user, user, sizeof(s.user));
            strncpy(s.local, local, sizeof(s.local));
            s.hora = hora;
            s.distancia = dist;
            s.estado = S_AGENDADO;
            strncpy(s.client_fifo, cf, sizeof(s.client_fifo));
            s.pid_veiculo = 0;
            servs[nserv++] = s;
            
            pthread_mutex_unlock(&serv_mutex);
            
            char resp[128]; 
            snprintf(resp, sizeof(resp), "OK agendado id=%d", s.id);
            enviar_cliente(cf, resp);
        }
    } else if(strcmp(cmd,"CONSULTAR")==0){
        char user[64], cf[128];
        if(sscanf(msg+9, "%63s %127s", user, cf)==2){
            char buf[LINEBUF];
            pthread_mutex_lock(&serv_mutex);
            for(int i=0;i<nserv;i++){
                if(strcmp(servs[i].user, user)==0){
                    snprintf(buf, sizeof(buf), "SERV id=%d hora=%d local=%s dist=%d estado=%d",
                             servs[i].id, servs[i].hora, servs[i].local, servs[i].distancia, servs[i].estado);
                    enviar_cliente(cf, buf);
                }
            }
            pthread_mutex_unlock(&serv_mutex);
            enviar_cliente(cf, "Consulta efetuada");
        }
    } else if (strcmp(cmd,"CANCELAR")==0){
        char user[64], cf[128];
        int id_servico; 
        
        // Tenta ler 3 argumentos: CANCELAR <user> <ID> <client_fifo>
        if(sscanf(msg+8, "%63s %d %127s", user, &id_servico, cf)==3){
            
            int found = 0;
            pthread_mutex_lock(&serv_mutex);
            
            for(int i=0;i<nserv;i++){
                if(strcmp(servs[i].user, user)==0){
                    // Se id_servico for 0, cancela todos os serviços deste utilizador
                    if(id_servico == 0 || servs[i].id == id_servico){
                        
                        // Estado é alterado para S_CANCELADO (2)
                        if(servs[i].estado == S_AGENDADO || servs[i].estado == S_EM_CURSO){
                            servs[i].estado = S_CANCELADO; 
                            found = 1;

                            if(servs[i].estado == S_EM_CURSO && servs[i].pid_veiculo>0){
                                kill(servs[i].pid_veiculo, SIGUSR1);
                            }
                        }
                        if(id_servico != 0 && servs[i].id == id_servico) break; 
                    }
                }
            }
            
            pthread_mutex_unlock(&serv_mutex);
            
            if(found) enviar_cliente(cf, "Servico cancelado com sucesso!");
            else enviar_cliente(cf, "ERRO não encontrado, ou já concluído/cancelado");
        } else {
             enviar_cliente(cf, "ERRO invalido cancelar comando");
        }
    }
}

/* argumentos para a thread de telemetria */
typedef struct {
    int read_fd;      // Descritor de leitura do pipe anónimo
    int service_id;   // ID do serviço associado
    pid_t veic_pid;   // PID do processo veículo
} tel_arg_t;

/* thread que lê telemetria do pipe do veículo */
void *telemetry_thread(void *arg){
    tel_arg_t *ta = (tel_arg_t*)arg;
    
    // Associa o file descriptor de leitura (pipe anónimo) a um stream FILE*
    FILE *rf = fdopen(ta->read_fd, "r");
    if(!rf){
        close(ta->read_fd);
        free(ta);
        return NULL;
    }
    char line[256];
    
    // Loop bloqueia em fgets() até que haja dados no pipe ou o pipe feche (veículo termina)
    while(fgets(line, sizeof(line), rf)){
        // Remove o newline (LF) no final da linha
        size_t L = strcspn(line, "\n"); line[L] = '\0';
        
        // --- DESCOBRIR DO PROGRESSO ---
        if(strncmp(line, "PROGRESS", 8) == 0){
            int percor = 0, total = 0;
            // Espera PROGRESS <percorridos>/<total>
            if(sscanf(line + 8, "%d/%d", &percor, &total) == 2 && total > 0){
                int perc = (percor * 100) / total;
                
                pthread_mutex_lock(&prog_mutex); // Usa o mutex da nova estrutura de progresso
                int idx = find_index_by_id(ta->service_id);
                if(idx >= 0 && idx < MAX_SERV){ // Verifica limites
                    progresso_servico[idx] = perc;
                }
                pthread_mutex_unlock(&prog_mutex);
            }
        }
        
        /* --- Atualização total_km --- */
        if(strncmp(line, "END_COMPLETED", 13) == 0){
            int km = 0;
            // Tenta ler o valor inteiro após "END_COMPLETED" e espaços
            if (sscanf(line + 13, "%d", &km) == 1){
                // Atualiza o total de km (já protegido por mutex interno)
                atualizar_total_km(km);
            }
        } 
        
        
    }
    
    // Fechar o stream FILE* (fecha automaticamente o file descriptor subjacente)
    fclose(rf); 
    
    // Libertar memória alocada para os argumentos da thread
    free(ta);
    
    return NULL;
}

/* lança veículo por índice (deve ser chamado com mutex quando necessário) */
/* Cria pipe, faz fork, e executa o binário './veiculo' */
void launch_vehicle_by_index(int idx){
    if(idx < 0) return;
    pthread_mutex_lock(&serv_mutex);
    if(veiculos_em_exec >= nveiculos || servs[idx].estado != S_AGENDADO){
        pthread_mutex_unlock(&serv_mutex);
        return;
    }
    /* prepara cópia de campos para usar fora do fork (evita races) */
    servico s_copy = servs[idx];
    /* marcamos como "em curso" antes do fork para evitar duplo-lançamento */
    servs[idx].estado = S_EM_CURSO;
    pthread_mutex_unlock(&serv_mutex);
    
    int pfd[2];
    if(pipe(pfd) < 0){
        perror("pipe");
        /* em caso de erro, restaurar estado para agendado */
        pthread_mutex_lock(&serv_mutex);
        /* tentar reverter (se ainda for o mesmo id) */
        int idx2 = find_index_by_id(s_copy.id);
        if(idx2>=0 && servs[idx2].id == s_copy.id) servs[idx2].estado = S_AGENDADO;
        pthread_mutex_unlock(&serv_mutex);
        return;
    }
    
    pid_t pid = fork();
    if(pid < 0){
        perror("fork");
        close(pfd[0]); close(pfd[1]);
        /* reverter estado */
        pthread_mutex_lock(&serv_mutex);
        int idx2 = find_index_by_id(s_copy.id);
        if(idx2>=0 && servs[idx2].id == s_copy.id) servs[idx2].estado = S_AGENDADO;
        pthread_mutex_unlock(&serv_mutex);
        return;
    }
    if(pid == 0){
        /* PROCESSO FILHO (VEÍCULO): redireciona stdout para pfd[1] e executa ./veiculo */
        close(pfd[0]);
        if(dup2(pfd[1], STDOUT_FILENO) < 0){
            perror("dup2");
            _exit(1);
        }
        close(pfd[1]);
        char dist_s[32], id_s[32];
        snprintf(dist_s, sizeof(dist_s), "%d", s_copy.distancia);
        snprintf(id_s, sizeof(id_s), "%d", s_copy.id);
        execl("./veiculo", "veiculo", s_copy.user, s_copy.client_fifo, dist_s, id_s, (char*)NULL);
        perror("execl veiculo");
        _exit(1);
    } else {
        /* PROCESSO PAI (CONTROLADOR) */
        close(pfd[1]); /* fecha ponta de escrita no pai */
        pthread_mutex_lock(&serv_mutex);
        /* atualiza pid e contador */
        int idx2 = find_index_by_id(s_copy.id);
        if(idx2>=0){
            servs[idx2].pid_veiculo = pid;
            servs[idx2].estado = S_EM_CURSO;
            veiculos_em_exec++;
        } else {
            /* se não encontrar, marca estado cancelado */
        }
        pthread_mutex_unlock(&serv_mutex);

        /* cria thread de telemetria que lê pfd[0] */
        tel_arg_t *ta = malloc(sizeof(tel_arg_t));
        if(!ta){
            close(pfd[0]);
            return;
        }
        ta->read_fd = pfd[0];
        ta->service_id = s_copy.id;
        ta->veic_pid = pid;
        pthread_t th;
        pthread_create(&th, NULL, telemetry_thread, ta);
        pthread_detach(th);
    }
}

/* verifica serviços agendados e recolhe ids para lançar */
/* Verifica se a hora simulada >= hora agendada e lança o veículo se houver vaga */
void check_launch_scheduled(){
    int ids[MAX_SERV];
    int cnt = 0;
    pthread_mutex_lock(&serv_mutex);
    for(int i=0;i<nserv;i++){
        if(servs[i].estado==S_AGENDADO && servs[i].hora <= simulated_time){
            ids[cnt++] = servs[i].id;
        }
    }
    pthread_mutex_unlock(&serv_mutex);

    for(int k=0;k<cnt;k++){
        /* para cada id, procura índice atual e lança */
        pthread_mutex_lock(&serv_mutex);
        int idx = find_index_by_id(ids[k]);
        if(idx>=0 && servs[idx].estado==S_AGENDADO && servs[idx].hora <= simulated_time){
            pthread_mutex_unlock(&serv_mutex);
            launch_vehicle_by_index(idx);
        } else {
            pthread_mutex_unlock(&serv_mutex);
        }
    }
}

/* thread que lê comandos do admin (bloqueante) */
/* Executa em paralelo para não bloquear o loop principal do controlador */
void *admin_thread(void *arg){
    (void)arg;
    /* tornar thread cancelável */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    char *line = NULL;
    size_t sz = 0;
    ssize_t nread;

    safe_printf("ADM> ");
    fflush(stdout);

    while(!terminar_flag && (nread = getline(&line, &sz, stdin)) != -1){
        if(nread>0 && line[nread-1]=='\n') line[nread-1] = '\0';
        trata_input_admin(line);
        if(terminar_flag) break;
        safe_printf("ADM> ");
        fflush(stdout);
    }
    free(line);
    return NULL;
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    // Configuração via variável de ambiente
    char *env = getenv("NVEICULOS");
    if(env) nveiculos = atoi(env);
    if(nveiculos<=0 || nveiculos>MAX_VEIC) nveiculos = MAX_VEIC;

    signal(SIGINT, sigint_handler); // Ctrl+C -> terminar_flag
    /* NÃO ignoramos SIGCHLD; vamos usar waitpid para actualizar estados */

    cria_fifo_server();

    /* abre fifo para ler (non-blocking) */
    int fd_server = open(SERVER_FIFO, O_RDONLY | O_NONBLOCK);
    if(fd_server < 0){ perror("Erro a abrir fifo do servidor"); exit(1); }

    /* abre dummy writer para evitar EOF quando não houver writers */
    int dummy = open(SERVER_FIFO, O_WRONLY | O_NONBLOCK);
    (void)dummy;

    start_time_unix = time(NULL);

    char readbuf[LINEBUF];
    ssize_t r;
    size_t rbuf_pos = 0;

    safe_printf("[SISTEMA] Controlador iniciado. NVEICULOS=%d\n", nveiculos);

    /* cria thread admin */
    if(pthread_create(&admin_tid, NULL, admin_thread, NULL) != 0){
        perror("Erro ao criar thread admin");
        exit(1);
    }

    /* LOOP PRINCIPAL DO CONTROLADOR */
    while(!terminar_flag){
        /* ler dados do fifo do servidor */
        r = read(fd_server, readbuf + rbuf_pos, sizeof(readbuf)-rbuf_pos-1);
        if(r>0){
            rbuf_pos += r;
            readbuf[rbuf_pos] = '\0';
            char *line = NULL;
            while((line = strchr(readbuf, '\n'))){
                *line = '\0';
                processa_pedido(readbuf);
                size_t rem = strlen(line+1);
                memmove(readbuf, line+1, rem+1);
                rbuf_pos = rem;
            }
        } else if(r==0){
          
            struct timespec req = {0, 100000000}; // Pausa de 0.1s para não gastar CPU em loop
            nanosleep(&req, NULL);
        } else {
            /* r < 0 */
            if(errno != EAGAIN && errno != EWOULDBLOCK){
                perror("Erro ao ler fifo servidor");
            }
        }

        /* avança o tempo simulado a cada segundo real (sleep(1)) */
        sleep(1);
        pthread_mutex_lock(&serv_mutex);
        simulated_time++;
        pthread_mutex_unlock(&serv_mutex);

        /* lança os agendados cuja hora chegou */
        check_launch_scheduled();

        /* waitpid com WNOHANG: verifica se algum filho morreu sem bloquear */
        int status;
        pid_t w;
        while((w = waitpid(-1, &status, WNOHANG)) > 0){
            pthread_mutex_lock(&serv_mutex);
            for(int i=0;i<nserv;i++){
                if(servs[i].pid_veiculo == w){
                    if(servs[i].estado == S_EM_CURSO){
                        servs[i].estado = S_CONCLUIDO;
                    }
                    servs[i].pid_veiculo = 0;
                    veiculos_em_exec = veiculos_em_exec>0?veiculos_em_exec-1:0;
                }
            }
            pthread_mutex_unlock(&serv_mutex);
        }
    }

    // --- LÓGICA DE FINALIZAÇÃO CORRIGIDA (USA utls_ligados) ---
    safe_printf("[SISTEMA] Controlador a terminar. Cancelando e notificando clientes...\n");
    
    pthread_mutex_lock(&utls_mutex);
    for(int i=0; i<MAX_UTIL_LIGADOS; i++){
        if(utls_ligados[i].ativo == 1){
            pid_t client_pid;
            // Extrai o PID do nome do FIFO: "/tmp/fifo_client_<PID>"
            if(sscanf(utls_ligados[i].client_fifo, "/tmp/fifo_client_%d", &client_pid) == 1){
                kill(client_pid, SIGTERM); // Envia sinal de terminação ao cliente
            }
            utls_ligados[i].ativo = 0; // Marca como inativo
        }
    }
    pthread_mutex_unlock(&utls_mutex);
    
    /* cancelar todos os serviços agendados ou em curso */
    cancelar_serv_id(0);

    /* acorda/termina thread admin se necessário */
    pthread_cancel(admin_tid);
    pthread_join(admin_tid, NULL);

    unlink(SERVER_FIFO);
    return 0;
}