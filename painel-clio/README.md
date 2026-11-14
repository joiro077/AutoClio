# Painel Clio / Twingo (ESP32-8048S070C + ELM327)

Firmware ESP-IDF para a tela de 7" ESP32-8048S070C (ESP32-S3, 800x480, touch GT911)
lendo a injeção pelo ELM327 **ligado por fio** (UART).

> Estado: a lógica de cálculo (consumo, viagem, marcha, troca, monitor de injeção,
> decodificação Renault/OBD2 e o parser do ELM) **compila e passa nos testes no PC**
> (`make -C test`). A parte ESP-IDF (tela, UART, NVS) foi escrita contra ESP-IDF 5.3/5.4,
> LVGL 9.2 e esp_lvgl_port 2.4, **mas ainda não foi compilada** (o ambiente onde escrevi
> não tinha acesso ao ESP-IDF). Espere alguns ajustes de API no primeiro `idf.py build`.

## Ligação

```
OBD (carro) ──> ELM327 v1.5 (sem o módulo Bluetooth) ──UART 38400──> ESP32-8048S070C
```

1. Abra o ELM327 e tire (ou desconecte) o módulo Bluetooth. Os pads onde ele estava
   são TX/RX da serial do PIC, em nível **3,3 V** (o módulo BT é 3,3 V). Confirme
   com o multímetro antes de ligar.
2. Ligações:

| ELM327 (pads do módulo BT)  | Placa ESP32-8048S070C          |
|-----------------------------|--------------------------------|
| TX do PIC (ia no RX do BT)  | **GPIO11** – conector P2, pino 2 |
| RX do PIC (vinha do TX do BT)| **GPIO17** – conector P5, pino 3 |
| GND                         | GND (qualquer conector)        |

   - Deixe o slot do cartão SD vazio (o GPIO11 também vai para ele).
   - Não use o GPIO18 para RX sem remover o R17 (0 Ω): ele está ligado ao INT do touch.
   - Os pinos podem ser trocados em `idf.py menuconfig` → *Painel Clio*.
3. Alimentação: a placa pede 5 V / ~500 mA. Use um conversor buck 12→5 V automotivo
   (≥2 A, com proteção). Não alimente pelo USB de um carregador de isqueiro barato.

### Opcional: relatório ao desligar a chave

Para mostrar o relatório **depois** de desligar a chave, a placa precisa continuar
alimentada alguns segundos. Monte um "power latch" (P-MOSFET no +12 V permanente,
ligado pelo pós-chave **ou** por um GPIO do ESP) e um divisor/opto lendo o pós-chave:

- `PAINEL_IGN_SENSE_GPIO` (sugestão: GPIO13, P2 pino 4) – entrada, 1 = chave ligada
- `PAINEL_POWER_HOLD_GPIO` (sugestão: GPIO12, P2 pino 3) – saída, 1 = segura a alimentação

Sem esse circuito, o relatório aparece quando o motor para com a chave ainda ligada
(rpm = 0), e some se a placa perder a alimentação.

## Compilar

```bash
. $IDF_PATH/export.sh          # ESP-IDF 5.3 ou mais novo
idf.py set-target esp32s3
idf.py build flash monitor
```

Na primeira compilação o gerenciador de componentes baixa LVGL, esp_lvgl_port e o
driver do GT911 (ver `main/idf_component.yml`).

Teste da lógica no PC (sem placa): `make -C test`

## Primeiro uso

1. **Menu → Modo demonstração: LIGADO** para ver a tela funcionando na bancada.
2. Desligue a demonstração, ligue no carro com a chave ligada. Em **Dados** aparece
   a ECU identificada e quantas leituras por segundo estão chegando.
3. Se aparecer "Desconhecida xxx/yyyy/zzzz", a ECU respondeu mas não está na tabela:
   mande esses números e o nome que o ddt4all mostra para gerar o perfil.
4. **Aprender marchas**: Menu → Aprender marcha → digite 1, rode uns 8 s estável em 1ª
   (acima de 15 km/h, sem embreagem). Repita para cada marcha.
5. **Calibrar consumo**: complete o tanque, toque *Abasteci: tanque cheio*. No próximo
   abastecimento (completando de novo), use *Calibrar consumo* com os litros que a bomba
   marcou. O fator é ajustado automaticamente.

## Estrutura

```
main/core/   C puro, testável no PC
  elm_parse     interpreta o texto do ELM327
  kwp_decode    respostas Renault 21xx (mesmas tramas do ddt4all)
  ecu_profiles_gen.c  GERADO pelo tools/ddt2profile.py (20 ECUs de Clio II/Twingo I)
  obd2_decode   SAE J1979: PIDs do modo 01 e DTCs (modo 03)
  fuel          consumo: ECU > tempo de injeção > MAF > MAP (speed-density)
  trip          viagem, média, autonomia, calibração
  gear          marcha por rpm/km/h, troca eco/sport, curva de torque aprendida
  injmon        monitor de tempo de injeção e correções de combustível
main/app/    ESP-IDF
  elm327        UART
  obd_task      conexão Renault/OBD2, leitura, DTC, simulador
  logic_task    computador de bordo a 10 Hz, NVS, motor liga/desliga
  display       painel RGB + GT911 + LVGL
  ui            telas
tools/ddt2profile.py  gera os perfis a partir dos JSON do ddt4all
test/                 testes no PC
```

### Gerar perfis de outras ECUs

```bash
mkdir /tmp/ecu && cd /tmp/ecu && unzip ecu.zip 'Sim32*' 'SIRIUS*'
python3 -I tools/ddt2profile.py main/core/ecu_profiles_gen.c /tmp/ecu/*.json
```
