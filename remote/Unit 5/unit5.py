
import random
from collections import deque
import gymnasium as gym
import numpy as np
from tensorflow import keras
from tqdm import tqdm

EPISODES = 800
GAMMA = 0.99
LR = 0.001
EPS_START = 1.0
EPS_MIN = 0.01
EPS_DECAY = 0.995
BATCH_SIZE = 32
MEMORY_SIZE = 100_000
TRAIN_START = 1_000
TARGET_UPDATE_EVERY = 100
TRAIN_FREQ = 1
PRINT_EVERY = 100
SOLVE_AVG = 170.0


def build_model(state_dim: int, action_dim: int):
    model = keras.Sequential([
        keras.layers.Input(shape=(state_dim,)),
        keras.layers.Dense(24, activation="relu",
                          kernel_initializer='he_uniform'),
        keras.layers.Dense(24, activation="relu",
                          kernel_initializer='he_uniform'),
        keras.layers.Dense(action_dim, activation="linear",
                          kernel_initializer='he_uniform'),
    ])
    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=LR),
        loss="huber"
    )
    return model


def choose_action(model, state, epsilon, action_dim):
    if np.random.rand() < epsilon:
        return np.random.randint(action_dim)
    q = model.predict(state[np.newaxis, :], verbose=0)[0]
    return int(np.argmax(q))


def train_step(model, target_model, memory, batch_size):
    if len(memory) < batch_size:
        return 0.0
    batch = random.sample(memory, batch_size)
    states = np.array([b[0] for b in batch], dtype=np.float32)
    actions = np.array([b[1] for b in batch], dtype=np.int32)
    rewards = np.array([b[2] for b in batch], dtype=np.float32)
    next_states = np.array([b[3] for b in batch], dtype=np.float32)
    dones = np.array([b[4] for b in batch], dtype=np.float32)
    current_q = model.predict(states, verbose=0)
    next_q = target_model.predict(next_states, verbose=0)
    max_next_q = np.max(next_q, axis=1)
    target_q = current_q.copy()
    for i in range(batch_size):
        if dones[i]:
            target_q[i, actions[i]] = rewards[i]
        else:
            target_q[i, actions[i]] = rewards[i] + GAMMA * max_next_q[i]

    history = model.fit(states, target_q, epochs=1, verbose=0)
    return history.history['loss'][0]

def main():
    env = gym.make("CartPole-v1")
    state_dim = int(env.observation_space.shape[0])
    action_dim = int(env.action_space.n)

    print(f"State dim: {state_dim}, Action dim: {action_dim}")
    print(f"Starting training\n")

    model = build_model(state_dim, action_dim)
    target_model = build_model(state_dim, action_dim)
    target_model.set_weights(model.get_weights())

    memory = deque(maxlen=MEMORY_SIZE)
    epsilon = EPS_START
    scores = []
    losses = []
    best_avg = 0.0

    for ep in tqdm(range(1, EPISODES + 1), desc="Training"):
        state, _ = env.reset()
        done = False
        total_reward = 0.0
        episode_loss = []

        while not done:
            action = choose_action(model, state, epsilon, action_dim)
            next_state, reward, terminated, truncated, _ = env.step(action)
            done = terminated or truncated
            memory.append((state, action, reward, next_state, float(terminated)))

            if len(memory) >= TRAIN_START:
                loss = train_step(model, target_model, memory, BATCH_SIZE)
                episode_loss.append(loss)

            state = next_state
            total_reward += reward

        scores.append(total_reward)
        if episode_loss:
            losses.append(np.mean(episode_loss))

        epsilon = max(EPS_MIN, epsilon * EPS_DECAY)

        if ep % TARGET_UPDATE_EVERY == 0:
            target_model.set_weights(model.get_weights())

        if ep % PRINT_EVERY == 0:
            recent_scores = scores[-PRINT_EVERY:]
            avg_score = np.mean(recent_scores)
            max_score = np.max(recent_scores)
            avg_loss = np.mean(losses[-PRINT_EVERY:]) if losses else 0

            best_avg = max(best_avg, avg_score)

            tqdm.write(f"Ep {ep:4d} | Avg: {avg_score:6.2f} | Max: {max_score:6.2f} | "
                  f"Eps: {epsilon:.3f} | Loss: {avg_loss:.4f} | Best: {best_avg:.2f}")

            if avg_score >= SOLVE_AVG:
                tqdm.write(f"\n SOLVED at episode {ep} Average: {avg_score:.2f}")
                break

    print(f"\nTraining finished. Best average: {best_avg:.2f}")
    env.close()

    print("\nTesting trained agent...")
    test_env = gym.make("CartPole-v1", render_mode="human")
    test_scores = []

    for r in range(5):
        state, _ = test_env.reset()
        done = False
        total = 0.0

        while not done:
            action = choose_action(model, state, epsilon=0.0, action_dim=action_dim)
            state, reward, terminated, truncated, _ = test_env.step(action)
            done = terminated or truncated
            total += reward

        test_scores.append(total)
        print(f"Test episode {r+1}: {total:.0f}")

    print(f"\nTest average: {np.mean(test_scores):.2f}")
    test_env.close()

    model.save("cartpole_dqn.keras")
    print("Model saved")

if __name__ == "__main__":
    main()