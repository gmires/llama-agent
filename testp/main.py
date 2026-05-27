import random

class GuessingGame:
    def __init__(self, upper_bound=100):
        self.secret_number = random.randint(1, upper_bound)
        self.attempts = 0

    def make_guess(self, guess):
        self.attempts += 1
        if guess < self.secret_number:
            print("Troppo basso! Prova di nuovo.")
        elif guess > self.secret_number:
            print("Troppo alto! Prova di nuovo.")
        else:
            print(f"Congratulazioni! Hai indovinato il numero {self.secret_number} in {self.attempts} tentativi.")
            return True
        return False

def main():
    print("--- Benvenuto al Gioco di Indovinare il Numero! ---")
    
    game = GuessingGame(upper_bound=100)
    
    print("Ho scelto un numero tra 1 e 100. Inizia a indovinare.")
    
    while True:
        try:
            guess = int(input("Inserisci il tuo indovino: "))
            
            if game.make_guess(guess):
                break
            
        except ValueError:
            print("Input non valido. Per favore, inserisci un numero intero.")

if __name__ == "__main__":
    main()