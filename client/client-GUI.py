from cv2_client import Cv2Player
import customtkinter as ctk
from tkinter import IntVar, PhotoImage, TclError
import sys
from os import path, environ
import re

VERSION = "v0.4"

def resourcePath(relative): # allow for pulling icon.ico, or other ui assets, whether in pyinstaller exe or loose scripts
    return path.join(environ.get("_MEIPASS2", path.abspath(".")), relative)

class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.geometry("750x400")
        self.player = None
        self.protocol("WM_DELETE_WINDOW", self._onClose)
        self.title("SimpleStreamer {}".format(VERSION))
        self.setIcon()

        self.tabContainer = ctk.CTkTabview(master=self)
        self.tabContainer.pack(padx=20, pady=20)
        self.clientTab = self.tabContainer.add("Client Options") # settings passed when creating client instance live here
        self.streamControls = self.tabContainer.add("Stream Controls") # settings changed whilst client instance is running live here

        # client startup options
        self.usb = ctk.CTkCheckBox(master=self.clientTab, text="USB streaming", onvalue=1, offvalue=0, command=self.conflictToggle) 
        self.usb.set(False)
        self.usb.grid(columnspan=2, row=0, column=0, padx=20, pady=10)
        
        ctk.CTkLabel(master=self.clientTab, text="Console IP:").grid(row=1, column=0)
        self.host = ctk.CTkEntry(master=self.clientTab, placeholder_text="e.g '192.168.8.200'")
        self.host.grid(row=1, column=1, padx=20, pady=10)
        
        self.audioEnabled = ctk.CTkCheckBox(master=self.clientTab, text="Stream audio?", onvalue=1, offvalue=0, command=self.conflictToggle)
        self.audioEnabled.set(True)
        self.audioEnabled.grid(columnspan=2, row=1, column=2, padx=20, pady=10)

        ctk.CTkLabel(master=self.clientTab, text="Starting volume:").grid(row=3, column=2)
        self.startVolume = ctk.CTkEntry(master=self.clientTab)
        self.startVolume.set("50")
        self.startVolume.grid(row=3, column=3, padx=20, pady=10)

        self.startMuted = ctk.CTkCheckBox(master=self.clientTab, text="Begin muted?", onvalue=1, offvalue=0)
        self.startMuted.set(False)
        self.startMuted.grid(columnspan=2, row=2, column=2, padx=20, pady=10)
        
        ctk.CTkLabel(master=self.clientTab, text="Video stream port:").grid(row=2, column=0)
        self.vidPort = ctk.CTkEntry(master=self.clientTab)
        self.vidPort.set("8080")
        self.vidPort.grid(row=2, column=1, padx=20, pady=10)

        ctk.CTkLabel(master=self.clientTab, text="Audio stream port:").grid(row=3, column=0)
        self.audPort = ctk.CTkEntry(master=self.clientTab)
        self.audPort.set("8081")
        self.audPort.grid(row=3, column=1, padx=20, pady=10)

        self.startBtn = ctk.CTkButton(master=self.clientTab, text="Start", command=self.verify)
        self.startBtn.grid(columnspan=4, row=4, column=0, padx=20, pady=10)
        


        # streamer options
        self.currVol = IntVar(value=50)
        self.volSlider = ctk.CTkSlider(master=self.streamControls, state="disabled", orientation="vertical", variable=self.currVol, from_=0, to=100, number_of_steps=100, scroll_step=0.05, command=self.updateVol) # type: ignore
        self.volLabel = ctk.CTkLabel(master=self.streamControls, text="50%")
        self.volSlider.set(50)
        self.volSlider.grid(row=0, column=0, padx=20)
        self.volLabel.grid(row=1, column=0, padx=20)
        
        self.muteBtn = ctk.CTkButton(master=self.streamControls, state="disabled", text="Toggle mute", command=self.toggleMute) # type: ignore
        self.muteBtn.grid(row=0, column=1, padx=20, pady=10)
        
        self.halfScale = ctk.CTkButton(master=self.streamControls, state="disabled", text="Toggle half-scale decoding", command=self.toggleHalfScale) # type: ignore
        self.halfScale.grid(row=1, column=1, padx=20, pady=10   )
        
        self.endBtn = ctk.CTkButton(master=self.streamControls, state="disabled", text="End stream", command=self.endStream)
        self.endBtn.grid(rowspan=2, row=0, column=2, padx=20)

    def verify(self):
        safe = True # assume proper values, >1 invalid means no stream start
        if not re.match(r'(\b25[0-5]|\b2[0-4][0-9]|\b[01]?[0-9][0-9]?)(\.(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)){3}', self.host.get()) and not self.usb.get():
            safe = False # wireless mode & bad ip
        try:
            if self.audioEnabled.get(): # if audio, wifi and usb count
                int(self.startVolume.get()) 
            if not self.usb.get(): # if wireless
                int(self.vidPort.get())
                if self.audioEnabled.get(): # if wireless AND audio
                    int(self.audPort.get())
        except ValueError:
            safe = False
        else:
            if self.audioEnabled.get(): # if audio, wifi and usb count
                if int(self.startVolume.get()) < 0:
                    self.startVolume.set("0")
                elif int(self.startVolume.get()) > 100:
                    self.startVolume.set("100")

            if not self.usb.get(): # if wireless
                if (int(self.vidPort.get()) < 0 or int(self.vidPort.get()) > 65535):
                    safe = False
                if self.audioEnabled.get(): # if wireless AND audio
                    if (int(self.audPort.get()) < 0 or int(self.audPort.get()) > 65535):
                        safe = False

        if safe:
            self.playerBuilder()

    def toggleMute(self):
        if self.player is not None:
            self.player.toggleMute()

    def toggleHalfScale(self):
        if self.player is not None:
            self.player.toggleHalfScale()

    def updateVol(self, volume):
        if self.player is not None:
            self.player.setVolume(volume)
        self.volLabel.configure(text="{}%".format(str(volume)))

    def _watchPlayer(self):
        if self.player is None:
            return
        if self.player.isRunning():
            self.after(200, self._watchPlayer)
        else:
            self.endStream()

    def _onClose(self):
        if self.player is not None:
            self.player.quit()
            self.player = None
        self.destroy()

    def endStream(self):
        if self.player is not None:
            self.player.quit()
            self.player = None
        
        for i in self.streamControls.winfo_children():
            i.configure(state="disabled")

        for i in self.clientTab.winfo_children():
            i.configure(state="normal")

        self.conflictToggle()
        self.tabContainer.set("Client Options")

    def conflictToggle(self):
            usbConflict = [self.host, self.vidPort, self.audPort]
            audConflict = [self.startVolume, self.startMuted, self.audPort]
            if self.usb.get() and self.audioEnabled.get(): #special case to deal with overlap
                self.host.configure(state="disabled")
                self.vidPort.configure(state="disabled")
                self.audPort.configure(state="disabled")
            else:
                if self.usb.get():
                    for i in usbConflict:
                        i.configure(state="disabled")
                else:
                    for i in usbConflict:
                        i.configure(state="normal")

                if self.audioEnabled.get():
                    for i in audConflict:
                        i.configure(state="normal")
                else:
                    for i in audConflict:
                        i.configure(state="disabled")

    def playerBuilder(self):

        #usb, no audio 
        if self.usb.get() and not self.audioEnabled.get():
            self.player = Cv2Player(host="usb", audio=False, verbose=False)
            self.halfScale.configure(state="normal")
            
        #usb + audio
        elif self.usb.get() and self.audioEnabled.get():
            self.player = Cv2Player(host="usb", volume=int(self.startVolume.get()), 
            muted=bool(self.startMuted.get()), verbose=False)
            
            volume = int(self.startVolume.get())
            self.currVol.set(volume)
            self.volLabel.configure(text="{}%".format(str(volume)))
            self.volSlider.set(volume)

            for i in self.streamControls.winfo_children():
                i.configure(state="normal")
            
        #wifi, no audio
        elif not self.usb.get() and not self.audioEnabled.get():
            self.player = Cv2Player(host=self.host.get(), audio=False, 
            port=int(self.vidPort.get()), verbose=False)
            self.halfScale.configure(state="normal")
            
            
        #wifi + audio
        else:
            self.player = Cv2Player(host=self.host.get(), volume=int(self.startVolume.get()), 
            muted=bool(self.startMuted.get()), port=int(self.vidPort.get()), 
            audio_port=int(self.audPort.get()), verbose=False)

            volume = int(self.startVolume.get())
            self.currVol.set(volume)
            self.volLabel.configure(text="{}%".format(str(volume)))
            self.volSlider.set(volume)
            
            for i in self.streamControls.winfo_children():
                i.configure(state="normal")


        for i in self.clientTab.winfo_children():
            i.configure(state="disabled")
        self.player.start()
        self.endBtn.configure(state="normal")
        self.tabContainer.set("Stream Controls")
        self.after(200, self._watchPlayer)

    def setIcon(self):
        try:
            if sys.platform.startswith("win"):
                self.iconbitmap(resourcePath("icon.ico"))
            else:
                self.icon_image = PhotoImage(file=resourcePath("icon.png"))
                self.iconphoto(True, self.icon_image)
        except TclError:
            pass # icon not a core functionality, no reason to care whether user chooses to download it


app = App()
app.mainloop()