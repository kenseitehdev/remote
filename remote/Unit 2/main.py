import numpy as np
import matplotlib.pyplot as plt
import tensorflow as tf
from tensorflow.keras import Sequential
from tensorflow.keras.layers import LSTM, Dense, Dropout
from tensorflow.keras.optimizers import Adam
import json

def save_history(history, path="history_6.json"):
    with open(path, "w") as f:
        json.dump(history.history, f)

def save_history_npz(history, path="history_6.npz"):
    np.savez(path, **history.history)

def load_mnist_npz(path="mnist.npz"):
    data = np.load(path)

    required = {"x_train", "y_train", "x_test", "y_test"}
    keys = set(data.files)
    print("Keys:", data.files)

    missing = required - keys
    if missing:
        raise KeyError(f"Missing keys in {path}: {missing}. Found keys: {keys}")

    x_train = data["x_train"]
    y_train = data["y_train"]
    x_test = data["x_test"]
    y_test = data["y_test"]
    return x_train, y_train, x_test, y_test


def plot_first_five(x_train, y_train):
    plt.figure(figsize=(10, 2))
    for i in range(5):
        plt.subplot(1, 5, i + 1)
        plt.imshow(x_train[i], cmap="gray")
        plt.title(str(y_train[i]))
        plt.axis("off")
    plt.tight_layout()
    plt.show()


def build_rnn_model(input_shape):

    model = Sequential([
        LSTM(128, return_sequences=True, activation="relu", input_shape=input_shape),

        Dropout(0.2),
        LSTM(32, return_sequences=True, activation="relu"),
        Dropout(0.2),
        LSTM(32, activation="relu"),
        Dropout(0.2),
        Dense(10, activation="softmax"),
    ])
    opt = Adam(learning_rate=1e-3)
    model.compile(
        optimizer=opt,
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )

    return model

def plot_loss(history, title="Training vs Validation Loss"):
    plt.figure()
    plt.plot(history.history["loss"], label="train_loss")
    plt.plot(history.history["val_loss"], label="val_loss")
    plt.title(title)
    plt.xlabel("Epoch")
    plt.ylabel("Loss")
    plt.legend()
    plt.show()


def main():
    x_train, y_train, x_test, y_test = load_mnist_npz("mnist.npz")

    print("Raw shapes:", x_train.shape, x_test.shape, y_train.shape, y_test.shape)
    x_train = x_train.astype(np.float32) / 255.0
    x_test = x_test.astype(np.float32) / 255.0

    sample_img = x_train[0].copy()
    sample_label = int(y_train[0])
    print("Shape of first train element:", x_train[0].shape)  
    model = build_rnn_model(input_shape=x_train.shape[1:])
    model.summary()
    history_6 = model.fit(
        x_train, y_train,
        epochs=6,
        validation_data=(x_test, y_test),
        batch_size=128,
        verbose=1
    )
    save_history(history_6, "history_6.json")
    plot_loss(history_6, title="Loss (epochs=6)")

    loss, acc = model.evaluate(x_test, y_test, verbose=0)
    print(f"Eval loss: {loss:.4f}")
    print(f"Eval accuracy: {acc*100:.2f}%")
    pred_probs = model.predict(sample_img[np.newaxis, ...], verbose=0)
    pred_class = int(np.argmax(pred_probs, axis=1)[0])
    print("Saved sample true label:", sample_label)
    print("Saved sample predicted:", pred_class)
    print("Prediction correct?:", pred_class == sample_label)

    plt.figure()
    plt.imshow(sample_img, cmap="gray")
    plt.title(f"True={sample_label}, Pred={pred_class}")
    plt.axis("off")
    plt.show()

    model.save("mnist_rnn.keras")
    print("Model saved as mnist_rnn.keras")

    model2 = build_rnn_model(input_shape=x_train.shape[1:])
    history_2 = model2.fit(
        x_train, y_train,
        epochs=2,
        validation_data=(x_test, y_test),
        batch_size=128,
        verbose=1
    )
    save_history(history_2, "history_2.json")
    plot_loss(history_2, title="Loss (epochs=2)")

    loss2, acc2 = model2.evaluate(x_test, y_test, verbose=0)
    print(f"(epochs=2) Eval loss: {loss2:.4f}")
    print(f"(epochs=2) Eval accuracy: {acc2*100:.2f}%")

  
if __name__ == "__main__":
    main()