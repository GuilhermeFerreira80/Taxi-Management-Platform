

#include "veiculo.h"
// Inclusões adicionais necessárias para FIFO e erros:
#include <sys/stat.h> // Para mkfifo e constantes de permissão
#include <errno.h>    // Para errno e EEXIST
#include <string.h>   // Para strncmp
#include <unistd.h>   // Para open, write, close

// --- Variáveis e Funções Globais ---
static volatile int cancelar = 0; // Flag para cancelamento imediato

// Handler para SIGUSR1: sinal enviado pelo controlador para cancelar viagem
void sigusr1_handler(int s){
    (void)s;
    cancelar = 1;
}
// ------------------------------------------------------------------


int main(int argc, char **argv){
    if(argc < 5){
        fprintf(stderr, "uso: %s <user> <client_fifo> <dist_km> <id>\n", argv[0]);
        return 1;
    }
    
    (void)argv[1]; // Suprime o warning 'user'
    
    char *client_fifo = argv[2];
    int dist = atoi(argv[3]);
    int id = atoi(argv[4]);
    
    signal(SIGUSR1, sigusr1_handler);

    // --- 1. Notificar Cliente que CHEGUEI e Viagem Inicia ---
    // Comunicação inicial: Veículo -> Cliente (via FIFO do cliente)
    

    
    
    // --- 2. Início Imediato da Viagem ---
    
    int percor = 0;
    int total = dist;

    // Loop de simulação da viagem
    while(!cancelar && percor < total){
        sleep(1); // Simula tempo a passar
        percor += 1;
        if(percor > total) percor = total;
        
        // Telemetria: Reporta progresso ao controlador via stdout (pipe)
        printf("PROGRESS %d/%d\n", percor, total);
        fflush(stdout);
    }

    // --- 3. Conclusão ---
    
    // Reabre o FIFO do cliente para notificação final (não bloqueante)
    int fd = open(client_fifo, O_WRONLY | O_NONBLOCK);
    
    if(cancelar){
        // Informa controlador via stdout
        printf("END_CANCELLED %d\n", percor);
        fflush(stdout);
        
        // Notificação de Cancelamento ao Cliente (via FIFO do cliente)
        if(fd >= 0){ 
            char msg_cancel[128];
            snprintf(msg_cancel, sizeof(msg_cancel), "VEICULO_CANCELADO %d %d", id, percor);
            write(fd, msg_cancel, strlen(msg_cancel));
            write(fd, "\n", 1);
        }
    } else {
        printf("END_COMPLETED %d\n", total); 
        fflush(stdout);

        // Notificação de Conclusão ao Cliente (via FIFO)
        if(fd >= 0){ 
            char msg_concluido[128];
            snprintf(msg_concluido, sizeof(msg_concluido), "Veículo id:%d com %dkm chegou ao destino!", id, total);
            write(fd, msg_concluido, strlen(msg_concluido));
            write(fd, "\n", 1);
        }
    }
    
    if(fd >= 0) close(fd);
    
    return 0; 
}
