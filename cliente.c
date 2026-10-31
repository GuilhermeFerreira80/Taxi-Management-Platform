
#include "cliente.h"
#include <sys/select.h> 
#include <signal.h>    // Para lidar com SIGTERM/SIGINT

#define STDIN_FD 0

// Variável global para verificar o serviço que pode estar 'à espera de entrar'
static int active_service_id = 0;
static volatile int terminar_cliente = 0; // Flag para sair do loop principal

// Envia mensagem para fifo do servidor (non-blocking)
/* Abre o FIFO do servidor, escreve a mensagem e fecha imediatamente */
void enviar_server(const char *msg){
    int fd = open(SERVER_FIFO, O_WRONLY | O_NONBLOCK);
    if(fd<0){
        perror("Erro ao abrir fifo do servidor");
        return;
    }
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    close(fd);
}

// Handler para sinais (usado pelo controlador para fechar o cliente)
/* Captura SIGTERM/SIGINT e define a flag de terminação */
void sigterm_handler(int s){ 
    (void)s;
    terminar_cliente = 1; 
}

// Trata comandos introduzidos pelo utilizador (stdin)
/* Analisa comandos locais (agendar, consultar, cancelar, terminar) e formata protocolo para o servidor */
void tratar_input_utilizador(const char *user, const char *fifo_name, char *line){
    char cmd[64];
    if(sscanf(line, "%63s", cmd) != 1) return;

    if(strcmp(cmd,"agendar")==0){
        int hora, dist;
        char local[64];
        /* formato: agendar <hora> <local> <distancia> */
        if(sscanf(line+7, "%d %63s %d", &hora, local, &dist) == 3){
            char msg[LINEBUF];
            snprintf(msg, sizeof(msg), "AGENDAR %s %d %s %d %s", user, hora, local, dist, fifo_name);
            enviar_server(msg);
        } else {
            printf("[SISTEMA] Uso: agendar <hora> <local> <distancia>\n");
        }
    }
    else if(strcmp(cmd,"consultar")==0){
        char msg[LINEBUF];
        snprintf(msg, sizeof(msg), "CONSULTAR %s %s", user, fifo_name);
        enviar_server(msg);
    }
    else if(strcmp(cmd,"cancelar")==0){
        int id;
        if(sscanf(line+8, "%d", &id) == 1){
            char msg[LINEBUF];
            snprintf(msg, sizeof(msg), "CANCELAR %s %d %s", user, id, fifo_name);
            enviar_server(msg);
            if(id == active_service_id || id == 0) active_service_id = 0; // Limpa o ID ativo se cancelado
        } else {
            printf("[SISTEMA] Uso: cancelar <id>\n");
        }
    }
    
    else if(strcmp(cmd,"terminar")==0){
    char cancel_msg[LINEBUF];
    // Formato: CANCELAR <user> 0 <client_fifo>
    snprintf(cancel_msg, sizeof(cancel_msg), "CANCELAR %s %d %s", user, 0, fifo_name); // ID = 0
    enviar_server(cancel_msg); 
    
    printf("[SISTEMA]  Enviando cancelamento de todos os seus serviços agendados. A terminar...\n");
    terminar_cliente = 1;
}
    else {
        printf("[SISTEMA] Comando desconhecido: %s\n", cmd);
    }
}

// Trata mensagens recebidas pelo client_fifo (controlador ou veículo)
/* Analisa respostas do servidor (OK, SERV, VEICULO_CHEGOU) */
void processar_msg_fifo(const char *user, char *line){
    char cmd[64];
    int id;

    if(sscanf(line, "%63s", cmd) != 1) return;

    if(strcmp(cmd, "OK") == 0){
        // Ex: OK agendado id=X
        if(sscanf(line + 3, "agendado id=%d", &id) == 1){
            printf("\n[SISTEMA] Serviço ID %d agendado com sucesso.\n", id);
        } else {
             printf("\n[SISTEMA] %s\n", line);
        }
    } else if(strcmp(cmd, "SERV") == 0){
        // Ex: SERV id=X hora=Y ... (do consultar)
        printf("\n[Consulta]: %s\n", line);
    } else if(strcmp(cmd, "VEICULO_CHEGOU") == 0){
        // Ex: VEICULO_CHEGOU 1
        if(sscanf(line + 15, "%d", &id) == 1){
             // Cliente assume que a viagem iniciou.
             printf("\n\n*** [ALERTA] Veículo ID %d chegou! Viagem iniciada. ***\n\n", id);
        }
    } else {
        printf("\n[SISTEMA] %s\n", line);
    }
}


int main(int argc, char **argv){
    if(argc < 2){
        fprintf(stderr, "uso: %s <username>\n", argv[0]);
        return 1;
    }
    // Verifica se o servidor está a correr tentando abrir o FIFO dele
    int fd_server_test = open(SERVER_FIFO, O_WRONLY | O_NONBLOCK);
    if(fd_server_test < 0){
        // Retorna 1 (erro) se não conseguir abrir o FIFO.
        fprintf(stderr, "[ERRO FATAL] O controlador (%s) não está em execução ou não pode ser contactado. A sair...\n", SERVER_FIFO);
        return 1;
    }
    close(fd_server_test);

    char *user = argv[1];
    pid_t pid = getpid();
    char fifo_name[128];
    snprintf(fifo_name, sizeof(fifo_name), "/tmp/fifo_client_%d", (int)pid);
    
    // --- Configuração e Sinais ---
    signal(SIGINT, sigterm_handler);  // Ctrl+C
    signal(SIGTERM, sigterm_handler); // Sinal do controlador
    
    // --- Criação do FIFO exclusivo deste cliente ---
    unlink(fifo_name);
    if(mkfifo(fifo_name, 0666) < 0){
        if(errno != EEXIST){ perror("mkfifo client"); return 1; }
    }

    // --- 1. Registo Inicial ---
    char msg[LINEBUF];
    snprintf(msg, sizeof(msg), "REGISTER %s %s", user, fifo_name);
    enviar_server(msg);
    printf("[SISTEMA] Cliente %s: Registad@. A aguardar comandos!\n", user);

    // --- 2. Abrir FIFO para leitura ---
    int fd_fifo = open(fifo_name, O_RDONLY);
    if(fd_fifo < 0){ perror("open client fifo"); unlink(fifo_name); return 1; }
    
    // Abre um dummy writer para evitar EOF (que faria o select retornar sempre pronto para leitura com 0 bytes)
    int dummy_writer = open(fifo_name, O_WRONLY | O_NONBLOCK);
    if(dummy_writer < 0) { perror("open dummy writer"); } 

    char input_line[LINEBUF];
    char fifo_buf[LINEBUF];
    ssize_t nread;
    fd_set read_fds;
    int max_fd = (fd_fifo > STDIN_FD) ? fd_fifo : STDIN_FD;

    printf("CLI> ");
    fflush(stdout);

    // --- Loop Principal com SELECT (Multiplexagem de I/O) ---
    while(!terminar_cliente){
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FD, &read_fds); // Monitoriza teclado
        FD_SET(fd_fifo, &read_fds);  // Monitoriza FIFO do cliente

        if(select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0){
            if(errno == EINTR) continue; // select interrompido por sinal
            perror("select");
            break; 
        }

        // A) Dados vindos do FIFO (Mensagens assíncronas)
        if(FD_ISSET(fd_fifo, &read_fds)){
            nread = read(fd_fifo, fifo_buf, sizeof(fifo_buf)-1);
            if(nread > 0){
                fifo_buf[nread] = '\0';
                char *line = strtok(fifo_buf, "\n");
                while(line){
                    processar_msg_fifo(user, line);
                    line = strtok(NULL, "\n");
                }
                printf("CLI> "); fflush(stdout);
            } else if (nread == 0) {
                 printf("\n[SISTEMA] Controlador fechou o FIFO. A sair...\n");
                 break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                 perror("read client fifo");
            }
        }

        // B) Dados vindos do STDIN (Comandos do utilizador)
        if(FD_ISSET(STDIN_FD, &read_fds)){
            if(fgets(input_line, sizeof(input_line), stdin) == NULL){
                // EOF no stdin (Ctrl+D)
                printf("\nSaindo por EOF...\n");
                break;
            }
            // Remove newline
            size_t len = strlen(input_line);
            if(len > 0 && input_line[len-1] == '\n') input_line[len-1] = '\0';
            
            tratar_input_utilizador(user, fifo_name, input_line);
            
            if(!terminar_cliente) {
                printf("CLI> "); fflush(stdout);
            }
        }
    }

    // --- Limpeza Final ---
    close(fd_fifo);
    close(dummy_writer);
    unlink(fifo_name);
    return 0;
}