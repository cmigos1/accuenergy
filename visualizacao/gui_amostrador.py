import sys
import tkinter as tk
from tkinter import ttk, messagebox
import serial
import serial.tools.list_ports
import threading
import matplotlib.pyplot as plt
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

class AmostradorGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("Amostrador AC - Interface Gráfica")
        self.root.geometry("1100x700")
        
        self.serial_port = None
        self.is_reading = False
        self.read_thread = None
        
        self.setup_ui()
        self.update_ports()
        
    def setup_ui(self):
        # Estilo geral (opcional, para dar uma melhorada)
        style = ttk.Style()
        style.theme_use('clam')
        
        # Configurando grid principal
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)
        
        # Frame da Esquerda (Controles e Calibração)
        control_frame = ttk.Frame(self.root, padding=(10, 10))
        control_frame.grid(row=0, column=0, sticky="nswe")
        
        # Conexão
        conn_frame = ttk.LabelFrame(control_frame, text="Conexão Serial", padding=(10, 10))
        conn_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Label(conn_frame, text="Porta:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self.port_cb = ttk.Combobox(conn_frame, width=15)
        self.port_cb.grid(row=0, column=1, padx=5, pady=5)
        
        ttk.Button(conn_frame, text="Atualizar", command=self.update_ports).grid(row=0, column=2, padx=5, pady=5)
        
        self.btn_connect = ttk.Button(conn_frame, text="Conectar", command=self.connect)
        self.btn_connect.grid(row=1, column=0, columnspan=2, pady=5, sticky="we", padx=2)
        
        self.btn_disconnect = ttk.Button(conn_frame, text="Desconectar", command=self.disconnect, state=tk.DISABLED)
        self.btn_disconnect.grid(row=1, column=2, pady=5, sticky="we", padx=2)
        
        self.status_var = tk.StringVar(value="Status: Desconectado")
        ttk.Label(conn_frame, textvariable=self.status_var, font=("Arial", 9, "bold")).grid(row=2, column=0, columnspan=3, pady=5, sticky="w")
        
        # Calibração
        calib_frame = ttk.LabelFrame(control_frame, text="Fatores de Calibração", padding=(10, 10))
        calib_frame.pack(fill=tk.X, pady=10)
        
        ttk.Label(calib_frame, text="Fator Tensão:").grid(row=0, column=0, padx=5, pady=5, sticky="w")
        self.v_calib_var = tk.DoubleVar(value=1.0)
        ttk.Entry(calib_frame, textvariable=self.v_calib_var, width=10).grid(row=0, column=1, padx=5, pady=5)
        
        ttk.Label(calib_frame, text="Fator Corrente:").grid(row=1, column=0, padx=5, pady=5, sticky="w")
        self.i_calib_var = tk.DoubleVar(value=1.0)
        ttk.Entry(calib_frame, textvariable=self.i_calib_var, width=10).grid(row=1, column=1, padx=5, pady=5)

        ttk.Label(calib_frame, text="Offset Tensão:").grid(row=2, column=0, padx=5, pady=5, sticky="w")
        self.v_offset_var = tk.DoubleVar(value=0.0)
        ttk.Entry(calib_frame, textvariable=self.v_offset_var, width=10).grid(row=2, column=1, padx=5, pady=5)

        ttk.Label(calib_frame, text="Offset Corrente:").grid(row=3, column=0, padx=5, pady=5, sticky="w")
        self.i_offset_var = tk.DoubleVar(value=0.0)
        ttk.Entry(calib_frame, textvariable=self.i_offset_var, width=10).grid(row=3, column=1, padx=5, pady=5)

        ttk.Label(calib_frame, text="*Sinal = (M * Fator) + Offset", font=("Arial", 8, "italic")).grid(row=4, column=0, columnspan=2, pady=5)
        
        # Frame da Direita (Dados e Gráfico)
        right_frame = ttk.Frame(self.root, padding=(10, 10))
        right_frame.grid(row=0, column=1, sticky="nswe")
        right_frame.columnconfigure(0, weight=1)
        right_frame.rowconfigure(1, weight=1)
        
        # Frame de Dados Medidos
        data_frame = ttk.LabelFrame(right_frame, text="Dados Medidos (Tempo Real)", padding=(10, 10))
        data_frame.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        
        for i in range(3):
            data_frame.columnconfigure(i, weight=1)
            
        self.lbl_v_rms = ttk.Label(data_frame, text="0.00 V", font=("Arial", 28, "bold"), foreground="#0052cc")
        self.lbl_v_rms.grid(row=0, column=0, pady=5)
        ttk.Label(data_frame, text="Tensão RMS", font=("Arial", 12)).grid(row=1, column=0)
        
        self.lbl_i_rms = ttk.Label(data_frame, text="0.00 A", font=("Arial", 28, "bold"), foreground="#d93025")
        self.lbl_i_rms.grid(row=0, column=1, pady=5)
        ttk.Label(data_frame, text="Corrente RMS", font=("Arial", 12)).grid(row=1, column=1)
        
        self.lbl_power = ttk.Label(data_frame, text="0.00 VA", font=("Arial", 28, "bold"), foreground="#0f9d58")
        self.lbl_power.grid(row=0, column=2, pady=5)
        ttk.Label(data_frame, text="Potência Aparente", font=("Arial", 12)).grid(row=1, column=2)
        
        # Frame do Gráfico
        graph_frame = ttk.Frame(right_frame)
        graph_frame.grid(row=1, column=0, sticky="nswe")
        
        self.fig, self.ax = plt.subplots(figsize=(6, 4))
        self.line_v, = self.ax.plot([], [], label='Tensão (V)', color='blue')
        self.line_i, = self.ax.plot([], [], label='Corrente (A)', color='red')
        self.ax.set_title("Onda Recebida em Tempo Real")
        self.ax.set_xlabel("Nº da Amostra")
        self.ax.set_ylabel("Amplitude")
        self.ax.legend()
        self.ax.grid(True)
        
        self.canvas = FigureCanvasTkAgg(self.fig, master=graph_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        
    def update_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_cb['values'] = ports
        if ports:
            self.port_cb.current(0)
            
    def connect(self):
        port = self.port_cb.get()
        if not port:
            messagebox.showerror("Erro", "Nenhuma porta serial selecionada!")
            return
            
        try:
            self.serial_port = serial.Serial(port, 115200, timeout=1)
            self.is_reading = True
            
            # Atualiza UI
            self.btn_connect.config(state=tk.DISABLED)
            self.btn_disconnect.config(state=tk.NORMAL)
            self.port_cb.config(state=tk.DISABLED)
            self.status_var.set(f"Status: Conectado ({port})")
            
            # Inicia thread de leitura
            self.read_thread = threading.Thread(target=self.read_serial_data, daemon=True)
            self.read_thread.start()
        except Exception as e:
            messagebox.showerror("Erro de Conexão", f"Não foi possível abrir a porta {port}.\n\n{str(e)}")
            
    def disconnect(self):
        self.is_reading = False
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()
            
        self.btn_connect.config(state=tk.NORMAL)
        self.btn_disconnect.config(state=tk.DISABLED)
        self.port_cb.config(state=tk.NORMAL)
        self.status_var.set("Status: Desconectado")
        
    def read_serial_data(self):
        while self.is_reading:
            try:
                if self.serial_port.in_waiting:
                    linha = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                    
                    if linha.startswith("START"):
                        partes = linha.split(",")
                        if len(partes) == 4:
                            v_raw_rms = float(partes[1])
                            i_raw_rms = float(partes[2])
                            num_samples = int(partes[3])
                            
                            v_calib = self.v_calib_var.get()
                            i_calib = self.i_calib_var.get()
                            v_offset = self.v_offset_var.get()
                            i_offset = self.i_offset_var.get()
                            
                            v_rms_calib = (v_raw_rms * v_calib) + v_offset
                            i_rms_calib = (i_raw_rms * i_calib) + i_offset
                            
                            v_data = []
                            i_data = []
                            
                            while True:
                                dados = self.serial_port.readline().decode('utf-8', errors='ignore').strip()
                                if not dados: continue
                                if dados == "END":
                                    break
                                try:
                                    val_v_raw, val_i_raw = map(float, dados.split(","))
                                    # Calibrando ponto a ponto
                                    v_data.append((val_v_raw * v_calib) + v_offset)
                                    i_data.append((val_i_raw * i_calib) + i_offset)
                                except ValueError:
                                    pass
                                    
                            # Atualiza a GUI com os últimos dados recebidos via interface thread-safe do tkinter
                            self.root.after(0, self.update_plot, v_data, i_data, v_rms_calib, i_rms_calib)
            except Exception as e:
                # Interrupções de fechamento da porta são silenciadas no log do console
                pass
                
    def update_plot(self, v_data, i_data, v_rms, i_rms):
        if len(v_data) > 0 and len(i_data) > 0:
            self.line_v.set_data(range(len(v_data)), v_data)
            self.line_i.set_data(range(len(i_data)), i_data)
            
            # Recalcula limites auto-scale
            self.ax.relim()
            self.ax.autoscale_view()
            
            # Atualiza Título
            self.ax.set_title("Onda Recebida em Tempo Real (Valores Calibrados)")
            
            # Atualiza Labels de Dados Medidos
            self.lbl_v_rms.config(text=f"{v_rms:.2f} V")
            self.lbl_i_rms.config(text=f"{i_rms:.2f} A")
            self.lbl_power.config(text=f"{(v_rms * i_rms):.2f} VA")
            
            self.canvas.draw()

if __name__ == "__main__":
    root = tk.Tk()
    app = AmostradorGUI(root)
    
    def on_closing():
        app.disconnect()
        root.quit()
        root.destroy()
        sys.exit(0)
        
    # Garante que ao fechar a janela, o processo também seja encerrado
    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()
