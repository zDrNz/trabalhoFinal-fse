# Nest Learning Thermostat — Réplica com ESP32 (Trabalho 3)

Réplica acadêmica simplificada do **Nest Learning Thermostat**, desenvolvida com **ESP32** para a disciplina de Fundamentos de Sistemas Embarcados (2026/1).

> 🎥 **Vídeo de demonstração:** https://youtu.be/A4VnaPbbhLk

## 1. Descrição do Produto

O projeto reproduz as principais funcionalidades de um termostato inteligente:

- Leitura de temperatura e umidade ambiente;
- Ajuste do setpoint (temperatura alvo) via encoder rotativo;
- Alternância entre modos de operação: `OFF`, `HEAT`, `COOL` e `AUTO`;
- Acionamento de aquecimento/resfriamento por relés, com **histerese** e **tempo mínimo de ciclo** (proteção contra chaveamento excessivo);
- Detecção de presença via sensor PIR, com modo **Auto-Away** (economia de energia quando não há ninguém no ambiente por um período prolongado);
- Interface local em display OLED;
- Interface remota via página web servida pelo próprio ESP32;
- Publicação de estado e recebimento de comandos via **MQTT**.

## 2. Componentes Utilizados

| Componente                          | Função                                              | Pino(s) no ESP32     |
|--------------------------------------|------------------------------------------------------|------------------------|
| ESP32 DevKit C                       | Controlador central                                  | —                      |
| DHT11                                | Sensor de temperatura e umidade                      | GPIO 4                 |
| Display OLED SSD1306 128x64 (I2C)    | Exibição local do estado do termostato               | SDA=21, SCL=22         |
| Encoder rotativo KY-040 (com botão)  | Ajuste do setpoint / troca de modo                   | CLK=32, DT=33, SW=25   |
| Sensor PIR HC-SR501                  | Detecção de presença                                 | GPIO 27                |
| Módulo relé 2 canais (ativo em LOW)  | Canal 1: aquecimento (W) / Canal 2: resfriamento (Y) | HEAT=26, COOL=14       |

## 3. Lógica de Controle

- **Histerese:** ±0,5 °C em torno do setpoint, evitando acionamentos repetidos próximos ao alvo.
- **Tempo mínimo de ciclo:** intervalo mínimo entre ligamentos do relé (ajustável no código; valor reduzido para facilitar testes/demonstração).
- **Auto-Away:** se o sensor PIR não detectar movimento por um período configurado, o sistema desliga aquecimento/resfriamento automaticamente, voltando ao normal assim que detecta presença novamente.
- Aquecimento e resfriamento nunca ficam ativos ao mesmo tempo.

## 4. Comunicação Wireless

O ESP32 se conecta a uma rede Wi-Fi e disponibiliza dois canais de comunicação:

### 4.1 Página Web local
Servida diretamente pelo ESP32 (`WebServer`), acessível pelo IP exibido no display OLED após a conexão Wi-Fi. Permite:
- Visualizar temperatura atual, umidade, setpoint e modo;
- Ajustar o setpoint (`+` / `−`);
- Trocar o modo de operação (`OFF`, `HEAT`, `COOL`, `AUTO`);
- Acompanhar o status de aquecimento/resfriamento e presença/Auto-Away.

### 4.2 MQTT
O dispositivo publica o estado (JSON) no tópico `nest/state` e recebe comandos no tópico `nest/cmd`. Os comandos aceitos são os mesmos utilizados pela interface web (`temp_up`, `temp_down`, `mode_off`, `mode_heat`, `mode_cool`, `mode_auto`, `mode_cycle`, `set:<valor>`).

## 5. Estrutura do Repositório

```
.
├── firmware/
│   └── nest_thermostat.ino   # Código-fonte do ESP32 (Arduino framework)
└── README.md
```

## 6. Como Compilar e Executar

### 6.1 Pré-requisitos
- [Arduino IDE](https://www.arduino.cc/en/software) com suporte à placa ESP32 instalado (Board Manager: `esp32` by Espressif Systems).
- Bibliotecas (via Library Manager):
  - `Adafruit GFX Library`
  - `Adafruit SSD1306`
  - `DHT sensor library` (Adafruit)
  - `PubSubClient`
  - `WiFi` e `WebServer` (já inclusas no core ESP32)

### 6.2 Configuração
1. Abra `firmware/nest_thermostat.ino` na Arduino IDE.
2. Configure as credenciais de Wi-Fi e o endereço do broker MQTT nas variáveis `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST` (e `MQTT_PORT`/`MQTT_USER`/`MQTT_PASS`, se necessário) no início do arquivo.
3. Conecte os componentes conforme a tabela de pinagem (seção 2).
4. Selecione a placa ESP32 correspondente e a porta serial correta.
5. Compile e faça o upload para o ESP32.

### 6.3 Execução
1. Ao ligar, o display mostrará o status da conexão Wi-Fi e, em caso de sucesso, o IP atribuído ao ESP32.
2. Acesse esse IP em um navegador na mesma rede para abrir a interface web.
3. Para testar via MQTT, publique/inscreva-se nos tópicos `nest/state` e `nest/cmd` em um broker (ex: Mosquitto, HiveMQ) configurado no código.
4. Use o encoder rotativo para ajustar o setpoint (clique curto alterna entre editar setpoint e editar modo).
5. Aproxime-se do sensor PIR para simular presença e observe o comportamento do modo Auto-Away após o período de inatividade configurado.

## 7. Vídeo de Demonstração

*(Insira aqui o link do vídeo do YouTube demonstrando o funcionamento completo do sistema)*
