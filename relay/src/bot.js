/** Names this bot registered before every command moved under /relay. */
const LEGACY_COMMANDS = ['pair', 'unpair', 'relay-status'];

/**
 * The one slash command this bot owns: /relay, with pair, unpair and status
 * subcommands. Subcommands keep a single entry in the command picker and leave
 * the top-level names free for any other bot.
 */
export function relayCommand({ SlashCommandBuilder, PermissionFlagsBits }) {
  return new SlashCommandBuilder()
    .setName('relay')
    .setDescription('Bridge a Discord channel with a Raspi Host game server')
    .setDefaultMemberPermissions(PermissionFlagsBits.ManageGuild)
    .addSubcommand((subcommand) => subcommand
      .setName('pair')
      .setDescription('Pair a channel with a game server and get a pairing code')
      .addChannelOption((option) => option
        .setName('channel')
        .setDescription('The channel game chat should be bridged to')
        .setRequired(true))
      .addStringOption((option) => option
        .setName('label')
        .setDescription('Optional note, e.g. the server name')))
    .addSubcommand((subcommand) => subcommand
      .setName('unpair')
      .setDescription('Remove the pairing of a channel and invalidate its code')
      .addChannelOption((option) => option
        .setName('channel')
        .setDescription('The channel to unpair')
        .setRequired(true)))
    .addSubcommand((subcommand) => subcommand
      .setName('status')
      .setDescription('Show which channels of this server are paired'))
    .toJSON();
}

/**
 * Registers /relay without rewriting anything else.
 *
 * `commands.set()` replaces the application's entire command list, so a second
 * bot sharing this token would lose its commands every time the relay restarts.
 * `create()` writes only the command it is given (and updates one with the same
 * name), and the only deletions are the three names this bot used to register.
 */
export async function registerCommands(manager, guildId, command, log = () => {}) {
  try {
    const existing = guildId ? await manager.fetch({ guildId }) : await manager.fetch();
    for (const stale of [...existing.values()]) {
      if (!LEGACY_COMMANDS.includes(stale.name)) {
        continue;
      }
      if (guildId) {
        await manager.delete(stale.id, guildId);
      } else {
        await manager.delete(stale.id);
      }
      log('bot', `removed the old /${stale.name} command`);
    }
  } catch (error) {
    // A missing Manage Commands scope must not stop /relay being registered.
    log('warn', `could not remove the old slash commands: ${error.message}`);
  }

  if (guildId) {
    await manager.create(command, guildId);
  } else {
    await manager.create(command);
  }
}

/**
 * Discord side of the bridge.
 *
 * Bot messages and webhooks are ignored when relaying, which is what stops the
 * game -> Discord -> game feedback loop.
 */
export async function createBot({ core, token, log = () => {}, onChange = () => {}, publicUrl = '', relayLocked = false }) {
  // Imported lazily so a missing `npm install` or a missing token produces a
  // clear message from index.js instead of a module-resolution stack trace.
  const {
    Client, Events, GatewayIntentBits, PermissionFlagsBits, SlashCommandBuilder,
  } = await import('discord.js');

  const client = new Client({
    intents: [
      GatewayIntentBits.Guilds,
      GatewayIntentBits.GuildMessages,
      GatewayIntentBits.MessageContent,
    ],
  });

  client.once(Events.ClientReady, async (readyClient) => {
    log('bot', `logged in as ${readyClient.user.tag}`);
    try {
      // Global commands: propagation can take a while. Set DISCORD_GUILD_ID to
      // register in one guild instead, which is instant while testing.
      await registerCommands(
        readyClient.application.commands,
        process.env.DISCORD_GUILD_ID || null,
        relayCommand({ SlashCommandBuilder, PermissionFlagsBits }),
        log,
      );
      log('bot', 'slash commands registered');
    } catch (error) {
      log('error', `failed to register slash commands: ${error.message}`);
    }
  });

  client.on(Events.MessageCreate, (message) => {
    if (message.author.bot || message.webhookId || !message.guildId) {
      return;
    }
    if (!core.pairingForChannel(message.channelId)) {
      return;
    }
    const name = message.member?.displayName ?? message.author.username;
    core.publish(message.channelId, {
      name,
      message: message.content,
      authorId: message.author.id,
    });
  });

  client.on(Events.InteractionCreate, async (interaction) => {
    if (!interaction.isChatInputCommand() || !interaction.inGuild()) {
      return;
    }
    const isManager = interaction.memberPermissions?.has(PermissionFlagsBits.ManageGuild);
    if (!isManager) {
      await interaction.reply({ content: 'You need the "Manage Server" permission to do that.', ephemeral: true });
      return;
    }

    try {
      if (interaction.commandName !== 'relay') {
        return;
      }
      const subcommand = interaction.options.getSubcommand();

      if (subcommand === 'pair') {
        const channel = interaction.options.getChannel('channel', true);
        const label = interaction.options.getString('label') ?? '';
        const { code } = core.createPairing({
          guildId: interaction.guildId,
          channelId: channel.id,
          label,
        });
        onChange();
        log('pair', `channel ${channel.id} (${interaction.guild.name}) paired`);
        await interaction.reply({
          ephemeral: true,
          content: [
            `Paired <#${channel.id}>. Enter these in the Raspi Host panel for that server:`,
            '',
            `**Relay URL:** \`${publicUrl || 'https://your-relay-url'}\``,
            `**Pairing code:** \`${code}\``,
            publicUrl ? '' : 'The relay owner has not set `publicUrl` in the relay config yet.',
            '',
            'The code is a secret: anyone who has it can read and write this channel\'s bridge. Use `/relay unpair` to revoke it.',
            relayLocked ? 'Note: this relay only accepts connections from Raspi Host servers.' : '',
          ].join('\n'),
        });
        return;
      }

      if (subcommand === 'unpair') {
        const channel = interaction.options.getChannel('channel', true);
        const removed = core.revokeByChannel(channel.id);
        if (removed) {
          onChange();
        }
        await interaction.reply({
          ephemeral: true,
          content: removed ? `Unpaired <#${channel.id}>; its code no longer works.` : 'That channel is not paired.',
        });
        return;
      }

      if (subcommand === 'status') {
        const paired = [...core.pairings.values()]
          .filter((pairing) => pairing.guildId === interaction.guildId)
          .map((pairing) => `• <#${pairing.channelId}> ${pairing.label ? `(${pairing.label})` : ''}`);
        await interaction.reply({
          ephemeral: true,
          content: paired.length > 0 ? `Paired channels:\n${paired.join('\n')}` : 'No channels are paired.',
        });
      }
    } catch (error) {
      log('error', `interaction failed: ${error.message}`);
      if (!interaction.replied) {
        await interaction.reply({ content: 'Something went wrong, check the relay logs.', ephemeral: true });
      }
    }
  });

  return {
    client,
    async start() {
      await client.login(token);
    },
    async stop() {
      await client.destroy();
    },
  };
}

/** Players cannot inject Discord markdown (or a fake system event). */
function safeName(name) {
  const text = String(name ?? 'Player')
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/[*_~`]/g, '')
    .trim()
    .slice(0, 64);
  return text || 'Player';
}

/**
 * Sends a game message into the paired Discord channel.
 *
 * Chat lines are prefixed with the player name; system lines (server started,
 * player joined, ...) are sent verbatim because the game server already
 * formatted them. Mention parsing is disabled on both so nothing a player
 * types can ping @everyone.
 */
export function createDiscordSender({ client, log = () => {} }) {
  return async function sendToDiscord(pairing, { name, message, system = false }) {
    const channel = await client.channels.fetch(pairing.channelId).catch(() => null);
    if (!channel || !channel.isTextBased()) {
      throw new Error(`channel ${pairing.channelId} is missing, not text based, or not visible to the bot`);
    }
    const content = system ? String(message) : `**${safeName(name)}**: ${message}`;
    await channel.send({ content, allowedMentions: { parse: [] } });
    log('relay', system ? 'delivered system line' : `delivered message from ${safeName(name)}`);
  };
}
