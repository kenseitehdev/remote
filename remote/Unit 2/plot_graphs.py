import json
import matplotlib.pyplot as plt

def load_history(path):
    with open(path, "r") as f:
        return json.load(f)

def plot_loss(hist, title):
    plt.figure()
    plt.plot(hist["loss"], label="train_loss")
    plt.plot(hist["val_loss"], label="val_loss")
    plt.title(title)
    plt.xlabel("Epoch")
    plt.ylabel("Loss")
    plt.legend()
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    h6 = load_history("history_6.json")
    plot_loss(h6, "Loss (epochs=6)")

    h2 = load_history("history_2.json")
    plot_loss(h2, "Loss (epochs=2)")