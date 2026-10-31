#ifndef CONTROLADOR_H
#define CONTROLADOR_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>

/* Definições de constantes para configuração do sistema */
#define SERVER_FIFO "fifo_controlador"
#define MAX_USERS 30
#define MAX_SERV 200
#define MAX_VEIC 10
#define LINEBUF 512
#define MAX_UTIL_LIGADOS MAX_USERS

/* Estados possíveis para um serviço de transporte */
typedef enum {
    S_AGENDADO,   // Serviço agendado mas veículo ainda não partiu
    S_EM_CURSO,   // Veículo em movimento
    S_CANCELADO,  // Cancelado pelo utilizador ou admin
    S_CONCLUIDO   // Viagem terminada com sucesso
} estado_serv;

/* Estrutura para gerir utilizadores conectados (para notificações de shutdown, etc) */
typedef struct {
    char client_fifo[128]; // Nome do FIFO do cliente (/tmp/fifo_client_PID)
    int ativo;             // Flag: 1 se ativo, 0 se inativo
} utilizador_ligado;

/* Estrutura principal que representa um pedido de viagem/serviço */
typedef struct {
    int id;                 // Identificador único do serviço
    char user[64];          // Nome do utilizador que pediu
    char client_fifo[128];  // FIFO para onde enviar respostas
    int hora;               // Hora agendada (em tempo simulado)
    char local[64];         // Local de origem
    int distancia;          // Distância total a percorrer
    estado_serv estado;     // Estado atual (enum)
    pid_t pid_veiculo;      // PID do processo veículo (se em curso), ou 0
} servico;

/* Funções públicas usadas no controlador */
void safe_printf(const char *fmt, ...);
void sigint_handler(int s);
void cria_fifo_server();
void enviar_cliente(const char *fifo, const char *msg);

void listar_servicos();
void remover_serv_por_id(int id);
void cancelar_serv_id(int id);
void imprime_utilizadores();
void imprime_frota();
void atualizar_total_km(int km);

void trata_input_admin(char *line);
void processa_pedido(char *msg);
int find_index_by_id(int id);

void *admin_thread(void *arg);
void *telemetry_thread(void *arg);

void launch_vehicle_by_index(int idx);
void check_launch_scheduled();

#endif