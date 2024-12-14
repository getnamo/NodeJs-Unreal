// Import required modules
const readline = require('readline');

// Create a readline interface for CLI input/output
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: '> ', // Sets the prompt symbol
});

// Function to process incoming messages
function processInput(input) {
  const trimmedInput = input.trim();

  // Handle commands or input
  if (trimmedInput === 'exit') {
    console.log('Exiting the script. Goodbye!');
    rl.close();
    process.exit(0); // Ensure the process exits
  } else {
    console.log(`You entered: "${trimmedInput}"`);

    // Example: Respond to specific input
    if (trimmedInput === 'hello') {
      console.log('Hello to you too!');
    } else if (trimmedInput === 'status') {
      console.log('The script is running smoothly.');
    } else {
      console.log('Unknown command. Type "exit" to quit.');
    }
  }
}

// Display a welcome message
console.log('Node.js CLI communication script is running.');
console.log('Type a command or message. Type "exit" to quit.');
rl.prompt();

// Listen for input events
rl.on('line', (input) => {
  processInput(input);
  rl.prompt(); // Re-display the prompt after processing input
});

// Handle CTRL+C (SIGINT)
rl.on('SIGINT', () => {
  console.log('\nCaught interrupt signal (CTRL+C). Exiting.');
  rl.close();
  process.exit(0);
});

// Optional: Handle unexpected errors
process.on('uncaughtException', (err) => {
  console.error('Unhandled exception:', err);
  process.exit(1); // Exit the script on error
});
