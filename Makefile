# Variáveis
CC = gcc
CFLAGS = -Wall -O2
LDFLAGS_CONTROLLER = -pthread

# Lista de todos os executáveis a construir
EXECS = controlador cliente veiculo

# Targets de compilação
.PHONY: all $(EXECS) clean

# Target principal: compila todos os programas
all: $(EXECS)

# Target: Compilar o controlador
controlador: controlador.c controlador.h
        $(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_CONTROLLER)
# Compilação original do controlador: gcc -Wall -O2 -pthread -o controlador controlador.c

# Target: Compilar o cliente
cliente: cliente.c cliente.h
        $(CC) $(CFLAGS) $< -o $@
# Compilação original do cliente: gcc -Wall -O2 -o cliente cliente.c

# Target: Compilar o veículo
veiculo: veiculo.c veiculo.h
        $(CC) $(CFLAGS) $< -o $@
# Compilação original do veículo: gcc -Wall -O2 -o veiculo veiculo.c

# Target: Limpar ficheiros temporários e executáveis
clean:
        rm -f $(EXECS) *.o